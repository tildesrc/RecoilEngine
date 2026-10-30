/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Shaders/Shader.h"
#include "Rendering/Shaders/ShaderHandler.h"
#include "Rendering/Shaders/LuaShaderContainer.h"
#include "Rendering/Shaders/GLSLCopyState.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GlobalRenderingInfo.h"
#include "Game/Camera.h"
#include "Rendering/Env/ISky.h"
#include "Game/CameraHandler.h"

#include "System/SafeUtil.h"
#include "System/StringUtil.h"

#include <regex>
#include "System/FileSystem/FileHandler.h"
#include "System/SpringHash.h"
#include "System/Log/ILog.h"

#include "System/Config/ConfigHandler.h"

#include <algorithm>
#ifdef DEBUG
	#include <cstring> // strncmp
#endif

#include "System/Misc/TracyDefs.h"



// #extension has to precede any non-preprocessor token, so it must be lifted out
// of the body and re-emitted ahead of anything the compat prelude declares.
static void ExtractGlslExtensions(std::string* src, std::string* extensions)
{
	RECOIL_DETAILED_TRACY_ZONE;

	for (size_t pos = src->find("#extension "); pos != std::string::npos; pos = src->find("#extension ", pos)) {
		// only hoist directives that start their line, not ones inside a comment
		// or a string further along
		if (pos > 0 && (*src)[pos - 1] != '\n') {
			pos += 1;
			continue;
		}

		const auto eol = std::min(src->find('\n', pos) + 1, src->size());

		*extensions += src->substr(pos, eol - pos);
		src->erase(pos, eol - pos);
	}
}

// Aliases the fixed-function builtins a core profile removed onto uniforms and
// attributes the engine supplies. gl_* names are reserved and cannot be
// redeclared, but a #define is substituted before that check, so the aliases go
// through where a declaration would not.
//
// This is what lets game-supplied shaders keep working: they cannot be rewritten
// by the engine, and they funnel through here like everything else.
static std::string GetGlslCompatPrelude(unsigned int type, const std::string& src)
{
	RECOIL_DETAILED_TRACY_ZONE;

	if (!globalRenderingInfo.glContextIsCore)
		return "";

	std::string p =
		"// ---- core-profile compatibility prelude ----\n"
		"uniform mat4 spring_ModelViewMatrix;\n"
		"uniform mat4 spring_ProjectionMatrix;\n"
		"uniform mat4 spring_ModelViewProjectionMatrix;\n"
		"uniform mat4 spring_ModelViewMatrixInverse;\n"
		"uniform mat4 spring_ProjectionMatrixInverse;\n"
		"uniform mat3 spring_NormalMatrix;\n"
		"#define gl_ModelViewMatrix spring_ModelViewMatrix\n"
		"#define gl_ProjectionMatrix spring_ProjectionMatrix\n"
		"#define gl_ModelViewProjectionMatrix spring_ModelViewProjectionMatrix\n"
		"#define gl_ModelViewMatrixInverse spring_ModelViewMatrixInverse\n"
		"#define gl_ProjectionMatrixInverse spring_ProjectionMatrixInverse\n"
		"uniform mat4 spring_ModelViewProjectionMatrixInverse;\n"
		"#define gl_ModelViewProjectionMatrixInverse spring_ModelViewProjectionMatrixInverse\n"
		"#define gl_NormalMatrix spring_NormalMatrix\n"
		"#define texture2D texture\n"
		"#define texture2DProj textureProj\n"
		"#define texture3D texture\n"
		"#define textureCube texture\n"
		// The legacy shadow lookups returned vec4 with the comparison result in
		// every channel, while texture()/textureProj() on a sampler2DShadow
		// return a bare float - so callers doing shadow2DProj(...).r need the
		// vec4 shape put back, or the swizzle is a compile error.
		"#define shadow2D(s, c) vec4(texture(s, c))\n"
		"#define shadow2DProj(s, c) vec4(textureProj(s, c))\n"
		// gl_Fog was a fixed-function state struct; mirror its shape so shaders
		// that read it still compile. Fed from the engine's fog state.
		"struct spring_FogParameters { vec4 color; float density; float start; float end; float scale; };\n"
		"uniform spring_FogParameters spring_Fog;\n"
		"#define gl_Fog spring_Fog\n"
		// same for gl_LightSource[]; mirrors gl_LightSourceParameters
		"struct spring_LightSourceParameters {\n"
		"	vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector;\n"
		"	vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff;\n"
		"	float constantAttenuation; float linearAttenuation; float quadraticAttenuation;\n"
		"};\n"
		"uniform spring_LightSourceParameters spring_LightSource[8];\n"
		"#define gl_LightSource spring_LightSource\n";

	switch (type) {
		case GL_VERTEX_SHADER: {
			p +=
				"#define varying out\n"
				"#define attribute in\n"
				"in vec4 spring_Vertex;\n"
				"in vec3 spring_Normal;\n"
				"in vec4 spring_Color;\n"
				"in vec4 spring_MultiTexCoord0;\n"
				"in vec4 spring_MultiTexCoord1;\n"
				"#define gl_Vertex spring_Vertex\n"
				"#define gl_Normal spring_Normal\n"
				"#define gl_Color spring_Color\n"
				"#define gl_MultiTexCoord0 spring_MultiTexCoord0\n"
				"#define gl_MultiTexCoord1 spring_MultiTexCoord1\n"
				"#define ftransform() (spring_ModelViewProjectionMatrix * spring_Vertex)\n"
				"out vec4 spring_TexCoord[8];\n"
				"#define gl_TexCoord spring_TexCoord\n"
				// gl_ClipVertex is gone from core (gl_ClipDistance replaces it).
				// Alias it to a plain variable so assignments compile; user
				// clip planes are simply not applied until the call sites that
				// want them are moved to gl_ClipDistance.
				"vec4 spring_ClipVertex;\n"
				"#define gl_ClipVertex spring_ClipVertex\n"
				"out vec4 spring_FrontColor;\n"
				"#define gl_FrontColor spring_FrontColor\n"
				"out float spring_FogFragCoord;\n"
				"#define gl_FogFragCoord spring_FogFragCoord\n";
		} break;
		case GL_FRAGMENT_SHADER: {
			p +=
				"#define varying in\n"
				"in vec4 spring_TexCoord[8];\n"
				"#define gl_TexCoord spring_TexCoord\n";

			// gl_FragColor and gl_FragData[0] are the same output, and shaders
			// may reference both from different #ifdef branches, so declare the
			// array once and alias gl_FragColor onto its first element rather
			// than emitting two outputs that would both want location 0.
			p +=
				"out vec4 spring_FragData[8];\n"
				"#define gl_FragData spring_FragData\n"
				"#define gl_FragColor spring_FragData[0]\n";

			p +=
				"in vec4 spring_FrontColor;\n"
				"#define gl_Color spring_FrontColor\n"
				"in float spring_FogFragCoord;\n"
				"#define gl_FogFragCoord spring_FogFragCoord\n";
		} break;
		default: break;
	}

	return (p + "// ---- end compatibility prelude ----\n");
}

// GLSL ES forbids initializers on uniforms, but both engine and game shaders use
// them for defaults ("always pass" alpha control, identity clip planes, ...), and
// game shaders cannot be edited by the engine. So strip them at assembly time and
// hand back what was stripped, for the caller to upload after linking.
struct StrippedUniformDefault {
	std::string name;
	std::string value;
};

static void StripGlslUniformInitializers(std::string* src, std::vector<StrippedUniformDefault>* defaults)
{
	RECOIL_DETAILED_TRACY_ZONE;

	static const std::regex re(R"(^([ \t]*uniform[ \t]+[A-Za-z0-9_]+[ \t]+([A-Za-z0-9_]+)[ \t]*)=([^;]*);)", std::regex::multiline);

	std::string out;
	auto begin = std::sregex_iterator(src->begin(), src->end(), re);
	auto end = std::sregex_iterator();
	size_t last = 0;

	for (auto it = begin; it != end; ++it) {
		const std::smatch& m = *it;

		out.append(*src, last, m.position(0) - last);
		out.append(m[1].str());
		out.append(";");

		last = m.position(0) + m.length(0);

		if (defaults != nullptr)
			defaults->emplace_back(StrippedUniformDefault{m[2].str(), m[3].str()});
	}

	out.append(*src, last, std::string::npos);
	*src = std::move(out);
}

// GLSL ES 300 has no interface blocks (they arrive in ES 3.1), but anonymous
// ones are just a grouping of varyings, so "out Data { vec4 a; vec2 b; };"
// flattens to "out vec4 a; out vec2 b;" with no change to how members are
// referenced. Named instances would need member accesses rewritten too; the
// engine does not use them, and this leaves them alone.
static void FlattenGlslInterfaceBlocks(std::string* src)
{
	RECOIL_DETAILED_TRACY_ZONE;

	static const std::regex re(R"((in|out)[ \t]+[A-Za-z0-9_]+[ \t]*\{([^}]*)\}[ \t]*;)");

	std::string out;
	auto begin = std::sregex_iterator(src->begin(), src->end(), re);
	auto end = std::sregex_iterator();
	size_t last = 0;

	for (auto it = begin; it != end; ++it) {
		const std::smatch& m = *it;

		out.append(*src, last, m.position(0) - last);
		last = m.position(0) + m.length(0);

		const std::string qualifier = m[1].str();
		std::string members = m[2].str();

		for (size_t pos = 0, nxt = 0; (nxt = members.find(';', pos)) != std::string::npos; pos = nxt + 1) {
			std::string decl = members.substr(pos, nxt - pos);

			// trim
			const size_t b = decl.find_first_not_of(" \t\r\n");
			const size_t e = decl.find_last_not_of(" \t\r\n");

			if (b == std::string::npos)
				continue;

			out.append(qualifier).append(" ").append(decl, b, e - b + 1).append(";\n");
		}
	}

	out.append(*src, last, std::string::npos);
	*src = std::move(out);
}

// Experimental: rewrite an assembled source to target GLSL ES 300 (what WebGL2
// speaks). Enabled with SPRING_DBG_ES_SHADERS so the output can be validated
// offline; there is no ES context to run against yet.
static bool WantGlslEsOutput()
{
	static const bool want = (getenv("SPRING_DBG_ES_SHADERS") != nullptr);
	return want;
}

/*****************************************************************/

#define LOG_SECTION_SHADER "Shader"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_SHADER)

// use the specific section for all LOG*() calls in this source file
#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_SHADER

/*****************************************************************/

CONFIG(bool, UseShaderCache).defaultValue(true).description("If already compiled shaders should be shared via a cache, reducing compiles of already compiled shaders.");


/*****************************************************************/

static bool glslIsValid(GLuint obj)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const bool isShader = glIsShader(obj);
	assert(glIsShader(obj) || glIsProgram(obj));

	GLint compiled;
	if (isShader)
		glGetShaderiv(obj, GL_COMPILE_STATUS, &compiled);
	else
		glGetProgramiv(obj, GL_LINK_STATUS, &compiled);

	return compiled;
}


static std::string glslGetLog(GLuint obj)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const bool isShader = glIsShader(obj);
	assert(glIsShader(obj) || glIsProgram(obj));

	int infologLength = 0;
	int maxLength = 0;

	if (isShader)
		glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &maxLength);
	else
		glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &maxLength);

	std::string infoLog;
	infoLog.resize(maxLength);

	if (isShader)
		glGetShaderInfoLog(obj, maxLength, &infologLength, &infoLog[0]);
	else
		glGetProgramInfoLog(obj, maxLength, &infologLength, &infoLog[0]);

	infoLog.resize(infologLength);
	return infoLog;
}

static bool ExtractGlslVersion(std::string* src, std::string* version)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto pos = src->find("#version ");

	if (pos != std::string::npos) {
		const auto eol = src->find('\n', pos) + 1;
		*version = src->substr(pos, eol - pos);
		src->erase(pos, eol - pos);
		return true;
	}
	return false;
}

/*****************************************************************/


namespace Shader {
	static NullShaderObject nullShaderObject_(0, "");
	static NullProgramObject nullProgramObject_("NullProgram");

	NullShaderObject* nullShaderObject = &nullShaderObject_;
	NullProgramObject* nullProgramObject = &nullProgramObject_;


	/*****************************************************************/

	unsigned int IShaderObject::GetHash() const {
		unsigned int hash = 127;
		hash = spring::LiteHash((const void*)   srcText.data(),    srcText.size(), hash); // srcTextHash is not worth it, only called on reload
		hash = spring::LiteHash((const void*)modDefStrs.data(), modDefStrs.size(), hash);
		hash = spring::LiteHash((const void*)rawDefStrs.data(), rawDefStrs.size(), hash); // rawDefStrsHash is not worth it, only called on reload
		return hash;
	}

	// Shared with Lua-created shaders (LuaShaders.cpp), which otherwise reach
	// the driver as raw source: a game's GLSL is typically written against the
	// compatibility profile and fails on a core one with "#version required and
	// missing" or "'varying' : syntax error".
	//
	// Normalises the version line of <srcStr> (removing it from the source, since
	// nothing may precede it) and returns the preamble to put in front: version,
	// then any hoisted #extension directives, then the prelude that aliases the
	// removed builtins. Outside a core profile this returns just the version line
	// the source already had, leaving behaviour unchanged.
	std::string BuildGlslCompatPreamble(unsigned int type, std::string& srcStr)
	{
		RECOIL_DETAILED_TRACY_ZONE;

		std::string versionStr;
		ExtractGlslVersion(&srcStr, &versionStr);

		if (!globalRenderingInfo.glContextIsCore) {
			if (!versionStr.empty())
				EnsureEndsWith(&versionStr, "\n");

			return versionStr;
		}

		if (!versionStr.empty()) {
			int verNum = 0;

			// below 1.40 is rejected outright, as is the "compatibility" token;
			// re-emitting just the number drops it and defaults to core
			if (sscanf(versionStr.c_str(), "#version %d", &verNum) == 1 && verNum > 0)
				versionStr = "#version " + IntToString((verNum < 140) ? 150 : verNum);
		} else {
			// a core profile requires an explicit #version
			versionStr = "#version 150";
		}

		std::string prelude = GetGlslCompatPrelude(type, srcStr);
		std::string extensionsStr;

		// #extension must precede any non-preprocessor token, so it cannot stay
		// below the prelude's declarations
		if (!prelude.empty())
			ExtractGlslExtensions(&srcStr, &extensionsStr);

		EnsureEndsWith(&versionStr, "\n");
		if (!extensionsStr.empty()) EnsureEndsWith(&extensionsStr, "\n");

		return versionStr + extensionsStr + prelude;
	}

	std::string IShaderObject::GetShaderSource(const std::string& fileName)
	{
		if (fileName.find("void main()") != std::string::npos)
			return fileName;

		std::string soPath = "shaders/" + fileName;
		std::string soSource = "";

		CFileHandler soFile(soPath);

		if (soFile.FileExists()) {
			soSource.resize(soFile.FileSize());
			soFile.Read(&soSource[0], soFile.FileSize());
			reloadRequested = false;
		}
		else {
			LOG_L(L_ERROR, "[%s] file not found \"%s\"", __FUNCTION__, soPath.c_str());
		}

		return soSource;
	}

	bool IShaderObject::ReloadFromDisk()
	{
		RECOIL_DETAILED_TRACY_ZONE;
		reloadRequested = true;
		std::string newText = GetShaderSource(srcFile);

		if (newText != srcText) {
			srcText = std::move(newText);
			return true;
		}

		return false;
	}


	/*****************************************************************/

	GLSLShaderObject::GLSLShaderObject(
		unsigned int shType,
		const std::string& shSrcFile,
		const std::string& shSrcDefs
	): IShaderObject(shType, shSrcFile, shSrcDefs)
	{ }

	GLSLShaderObject::CompiledShaderObjectUniquePtr GLSLShaderObject::CompileShaderObject()
	{
		CompiledShaderObjectUniquePtr res(new CompiledShaderObject(), [](CompiledShaderObject* so) {
			glDeleteShader(so->id);
			so->id = 0;
			spring::SafeDelete(so);
		});

		assert(!srcText.empty());

		std::string sourceStr = srcText;
		std::string compatPrelude;
		std::string defFlags  = rawDefStrs + "\n" + modDefStrs;
		std::string versionStr;

		// extract #version pragma and put it on the first line (only allowed there)
		// version pragma in definitions overrides version pragma in source (if any)
		ExtractGlslVersion(&sourceStr, &versionStr);
		ExtractGlslVersion(&defFlags,  &versionStr);

		// A core profile rejects GLSL versions below 1.40 outright ("version '130'
		// is not supported"), so a shader written in modern syntax but still
		// declaring 130 cannot compile there at all. Raise those to 150, which
		// every core profile supports (core profiles start at GL 3.2). Shaders
		// that genuinely rely on removed builtins will now fail on those instead
		// of on the version line, which is the more useful error anyway.
		// Compatibility contexts keep whatever the shader asked for.
		if (globalRenderingInfo.glContextIsCore && !versionStr.empty()) {
			int verNum = 0;

			if (sscanf(versionStr.c_str(), "#version %d", &verNum) == 1 && verNum > 0) {
				// Anything below 1.40 is rejected outright, and so is the
				// "compatibility" profile token whatever version it is attached
				// to. Re-emitting just the number drops the token and defaults to
				// core, which is what we are running.
				if (verNum < 140)
					verNum = 150;

				versionStr = "#version " + IntToString(verNum);
			}
		}

		compatPrelude = GetGlslCompatPrelude(type, sourceStr);

		// A core profile requires an explicit #version; shaders that relied on
		// the default (110) do not compile at all without one.
		if (globalRenderingInfo.glContextIsCore && versionStr.empty())
			versionStr = "#version 150";

		std::string esPrecision;
		std::vector<StrippedUniformDefault> strippedDefaults;

		if (WantGlslEsOutput()) {
			// WebGL2 speaks GLSL ES 300, which needs the es-suffixed version,
			// mandatory precision qualifiers, and no uniform initializers.
			versionStr = "#version 300 es";
			esPrecision =
				"precision highp float;\n"
				"precision highp int;\n"
				"precision highp sampler2D;\n"
				"precision highp samplerCube;\n"
				"precision highp sampler2DShadow;\n";

			// NOTE: strippedDefaults carries what each uniform was initialised to.
			// Applying it needs a post-link pass that uploads those values once,
			// which is not written yet - there is no ES context to run against,
			// so nothing would exercise it. Until then an ES build would see
			// those uniforms default to zero rather than to the shader's value.
			StripGlslUniformInitializers(&sourceStr, &strippedDefaults);
			StripGlslUniformInitializers(&compatPrelude, &strippedDefaults);

			FlattenGlslInterfaceBlocks(&sourceStr);
			FlattenGlslInterfaceBlocks(&compatPrelude);
		}

		// #extension must precede every non-preprocessor token, and the compat
		// prelude below declares uniforms, so the directives have to be lifted
		// out of the body and emitted between the version and the prelude.
		std::string extensionsStr;

		if (!compatPrelude.empty()) {
			ExtractGlslExtensions(&sourceStr, &extensionsStr);
			ExtractGlslExtensions(&defFlags,  &extensionsStr);
		}

		if (!versionStr.empty())    EnsureEndsWith(&versionStr,    "\n");
		if (!esPrecision.empty())   EnsureEndsWith(&esPrecision,   "\n");
		if (!extensionsStr.empty()) EnsureEndsWith(&extensionsStr, "\n");
		if (!defFlags.empty())      EnsureEndsWith(&defFlags,      "\n");

		// NOTE: versionStr has to come first with nothing at all ahead of it. The
		// GLSL spec does allow comments before #version, but Apple's compiler
		// rejects them ("syntax error: #version"), so do not emit a banner here.
		std::vector<const GLchar*> sources = {
			versionStr.c_str(),
			extensionsStr.c_str(),
			esPrecision.c_str(),
			compatPrelude.c_str(),
			"// SHADER FLAGS\n",
			defFlags.c_str(),
			"// SHADER SOURCE\n",
			"#line 1\n",
			sourceStr.c_str()
		};

		res->id = glCreateShader(type);

		glShaderSource(res->id, sources.size(), &sources[0], NULL);
		glCompileShader(res->id);

		res->valid = glslIsValid(res->id);
		res->log   = glslGetLog(res->id);

		if (!res->valid && logReporting) {
			const std::string& name = srcFile.find("void main()") != std::string::npos ? "unknown" : srcFile;
			LOG_L(L_WARNING, "[GLSL-SO::%s] shader-object name: %s, compile-log:\n%s\n", __FUNCTION__, name.c_str(), res->log.c_str());

			// concatenate rather than indexing fixed positions, so this cannot
			// read out of bounds if the source list above ever changes length
			std::string fullSrc;
			for (const GLchar* src: sources)
				fullSrc += src;

			LOG_L(L_WARNING, "\n%s", fullSrc.c_str());
		}

		return res;
	}




	/*****************************************************************/

	IProgramObject::IProgramObject(const std::string& poName)
		: name(poName)
		, objID(0)
		, logReporting(true)
		, valid(false)
		, bound(false) {
	}

	void IProgramObject::SetLogReporting(bool b, bool shObjects)
	{
		RECOIL_DETAILED_TRACY_ZONE;
		logReporting = b;
		if (shObjects) {
			for (IShaderObject*& so : shaderObjs) {
				so->SetLogReporting(b);
			}
		}
	}

	void IProgramObject::Release() {
		RECOIL_DETAILED_TRACY_ZONE;
		for (IShaderObject*& so: shaderObjs) {
			so->Release();
			delete so;
		}

		uniformStates.clear();
		shaderObjs.clear();
		luaTextures.clear();
		log.clear();

		valid = false;
	}

	void IProgramObject::AttachShaderObject(IShaderObject* so)
	{
#ifdef _DEBUG
	#ifndef HEADLESS
		const auto it = std::find_if(shaderObjs.cbegin(), shaderObjs.cend(), [type = so->GetType()](Shader::IShaderObject* so) {
			return so->GetType() == type;
		});
		assert(it == shaderObjs.cend());
	#endif
#endif
		shaderObjs.push_back(so);
	}

	bool IProgramObject::RemoveShaderObject(GLenum soType) {
		RECOIL_DETAILED_TRACY_ZONE;
		for (size_t i = 0; i < shaderObjs.size(); ++i) {
			IShaderObject*& so = shaderObjs[i];
			if (so->GetType() == soType) {
				so->Release();
				delete so;

				shaderObjs.erase(shaderObjs.begin() + i);
				return true;
			}
		}

		return false;
	}

	void IProgramObject::Enable()
	{
		RECOIL_DETAILED_TRACY_ZONE;
		assert(!bound);
		shaderHandler->SetCurrentlyBoundProgram(this);
		bound = true;
	}

	void IProgramObject::Disable()
	{
		RECOIL_DETAILED_TRACY_ZONE;
		assert(bound);
		shaderHandler->SetCurrentlyBoundProgram(nullptr);
		bound = false;
	}

	bool IProgramObject::LoadFromLua(const std::string& filename) {
		RECOIL_DETAILED_TRACY_ZONE;
		return Shader::LoadFromLua(this, filename);
	}

	void IProgramObject::RecompileIfNeeded(bool validate)
	{
		RECOIL_DETAILED_TRACY_ZONE;
		if (shaderFlags.HashSet() && !shaderFlags.Updated())
			return;

		Reload(!shaderFlags.HashSet(), validate);
		PrintDebugInfo();
	}

	void IProgramObject::PrintDebugInfo()
	{
	#if DEBUG
		LOG_L(L_DEBUG, "Uniform States for program-object \"%s\":", name.c_str());
		LOG_L(L_DEBUG, "Defs:\n %s", (shaderFlags.GetString()).c_str());
		LOG_L(L_DEBUG, "Uniforms:");

		for (const auto& p : uniformStates) {
			const bool curUsed = GetUniformLocation(p.second.GetName()) >= 0;
			if (!p.second.IsInitialized()) {
				LOG_L(L_DEBUG, "\t%s: uninitialized used=%i", (p.second.GetName()), int(curUsed));
			} else {
				LOG_L(L_DEBUG, "\t%s: x=float:%f;int:%i y=%f z=%f used=%i", (p.second.GetName()), p.second.GetFltValues()[0], p.second.GetIntValues()[0], p.second.GetFltValues()[1], p.second.GetFltValues()[2], int(curUsed));
			}
		}
	#endif
	}

	UniformState* IProgramObject::GetNewUniformState(const char* name)
	{
		RECOIL_DETAILED_TRACY_ZONE;
		const auto hash = hashString(name);
		const auto it = uniformStates.emplace(hash, UniformState{name});

		UniformState* us = &(it.first->second);
		us->SetLocation(GetUniformLoc(name));

		return us;
	}


	void IProgramObject::AddTextureBinding(const int texUnit, const std::string& luaTexName)
	{
		RECOIL_DETAILED_TRACY_ZONE;
		LuaMatTexture luaTex;

		if (!LuaOpenGLUtils::ParseTextureImage(nullptr, luaTex, luaTexName))
			return;

		luaTextures[texUnit] = luaTex;
	}

	void IProgramObject::BindTextures() const
	{
		RECOIL_DETAILED_TRACY_ZONE;
		for (const auto& [relSlot, lmt] : luaTextures) {
			glActiveTexture(GL_TEXTURE0 + relSlot);
			lmt.Bind();
		}
		glActiveTexture(GL_TEXTURE0);
	}

	void IProgramObject::UnbindTextures() const
	{
		RECOIL_DETAILED_TRACY_ZONE;
		for (const auto& [relSlot, lmt] : luaTextures) {
			glActiveTexture(GL_TEXTURE0 + relSlot);
			lmt.Unbind();
		}
		glActiveTexture(GL_TEXTURE0);
	}




	/*****************************************************************/

	ARBProgramObject::ARBProgramObject(const std::string& poName): IProgramObject(poName) {
		objID = -1; // not used for ARBProgramObject instances
		uniformTarget = -1;
	}

	void ARBProgramObject::SetUniformTarget(int target) {
		RECOIL_DETAILED_TRACY_ZONE;
		uniformTarget = target;
	}
	int ARBProgramObject::GetUnitformTarget() {
		RECOIL_DETAILED_TRACY_ZONE;
		return uniformTarget;
	}

	void ARBProgramObject::Enable() {
		RECOIL_DETAILED_TRACY_ZONE;
		RecompileIfNeeded(true);
		for (const IShaderObject* so: shaderObjs) {
			glEnable(so->GetType());
			glBindProgramARB(so->GetType(), so->GetObjID());
		}
		IProgramObject::Enable();
	}
	void ARBProgramObject::Disable() {
		RECOIL_DETAILED_TRACY_ZONE;
		for (const IShaderObject* so: shaderObjs) {
			glBindProgramARB(so->GetType(), 0);
			glDisable(so->GetType());
		}
		IProgramObject::Disable();
	}

	void ARBProgramObject::Link() {
		RECOIL_DETAILED_TRACY_ZONE;
		RecompileIfNeeded(false);
		valid = true;

		for (const IShaderObject* so: shaderObjs) {
			valid &= so->IsValid();
		}
	}
	void ARBProgramObject::Reload(bool reloadFromDisk, bool validate) {
		RECOIL_DETAILED_TRACY_ZONE;
		for (IShaderObject* so: GetAttachedShaderObjs()) {
			if (reloadFromDisk) so->ReloadFromDisk();
			so->Compile();
		}

		// make HashSet() true
		shaderFlags.UpdateHash();

		if (validate) Validate();
	}


	void ARBProgramObject::SetUniform1i(int idx, int   v0                              ) { glProgramEnvParameter4fARB(uniformTarget, idx, float(v0), float( 0), float( 0), float( 0)); }
	void ARBProgramObject::SetUniform2i(int idx, int   v0, int   v1                    ) { glProgramEnvParameter4fARB(uniformTarget, idx, float(v0), float(v1), float( 0), float( 0)); }
	void ARBProgramObject::SetUniform3i(int idx, int   v0, int   v1, int   v2          ) { glProgramEnvParameter4fARB(uniformTarget, idx, float(v0), float(v1), float(v2), float( 0)); }
	void ARBProgramObject::SetUniform4i(int idx, int   v0, int   v1, int   v2, int   v3) { glProgramEnvParameter4fARB(uniformTarget, idx, float(v0), float(v1), float(v2), float(v3)); }
	void ARBProgramObject::SetUniform1f(int idx, float v0                              ) { glProgramEnvParameter4fARB(uniformTarget, idx, v0, 0.0f, 0.0f, 0.0f); }
	void ARBProgramObject::SetUniform2f(int idx, float v0, float v1                    ) { glProgramEnvParameter4fARB(uniformTarget, idx, v0,   v1, 0.0f, 0.0f); }
	void ARBProgramObject::SetUniform3f(int idx, float v0, float v1, float v2          ) { glProgramEnvParameter4fARB(uniformTarget, idx, v0,   v1,   v2, 0.0f); }
	void ARBProgramObject::SetUniform4f(int idx, float v0, float v1, float v2, float v3) { glProgramEnvParameter4fARB(uniformTarget, idx, v0,   v1,   v2,   v3); }

	void ARBProgramObject::SetUniform2iv(int idx, const int*   v) { int   vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] =    0; vv[3] =    0; glProgramEnvParameter4fvARB(uniformTarget, idx, (float*) vv); }
	void ARBProgramObject::SetUniform3iv(int idx, const int*   v) { int   vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] = v[2]; vv[3] =    0; glProgramEnvParameter4fvARB(uniformTarget, idx, (float*) vv); }
	void ARBProgramObject::SetUniform4iv(int idx, const int*   v) { int   vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] = v[2]; vv[3] = v[3]; glProgramEnvParameter4fvARB(uniformTarget, idx, (float*) vv); }
	void ARBProgramObject::SetUniform2fv(int idx, const float* v) { float vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] = 0.0f; vv[3] = 0.0f; glProgramEnvParameter4fvARB(uniformTarget, idx,          vv); }
	void ARBProgramObject::SetUniform3fv(int idx, const float* v) { float vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] = v[2]; vv[3] = 0.0f; glProgramEnvParameter4fvARB(uniformTarget, idx,          vv); }
	void ARBProgramObject::SetUniform4fv(int idx, const float* v) { float vv[4]; vv[0] = v[0]; vv[1] = v[1]; vv[2] = v[2]; vv[3] = v[3]; glProgramEnvParameter4fvARB(uniformTarget, idx,          vv); }




	/*****************************************************************/

	GLSLProgramObject::GLSLProgramObject(const std::string& poName): IProgramObject(poName), curSrcHash(0) {
		objID = glCreateProgram();
	}

	void GLSLProgramObject::BindAttribLocation(const std::string& name, uint32_t index)
	{
		attribLocations[name] = index;
	}

	void GLSLProgramObject::BindOutputLocation(const std::string& name, uint32_t index)
	{
		outputLocations[name] = index;
	}

	void GLSLProgramObject::Enable() {
		RECOIL_DETAILED_TRACY_ZONE;
		RecompileIfNeeded(true);
		EnableRaw();
	}

	void GLSLProgramObject::SetCompatUniforms() {
		RECOIL_DETAILED_TRACY_ZONE;

		if (!globalRenderingInfo.glContextIsCore || objID == 0)
			return;

		if (!compatLocs.resolved) {
			compatLocs.resolved = true;
			compatLocs.modelView        = glGetUniformLocation(objID, "spring_ModelViewMatrix");
			compatLocs.proj             = glGetUniformLocation(objID, "spring_ProjectionMatrix");
			compatLocs.modelViewProj    = glGetUniformLocation(objID, "spring_ModelViewProjectionMatrix");
			compatLocs.modelViewInv     = glGetUniformLocation(objID, "spring_ModelViewMatrixInverse");
			compatLocs.projInv          = glGetUniformLocation(objID, "spring_ProjectionMatrixInverse");
			compatLocs.modelViewProjInv = glGetUniformLocation(objID, "spring_ModelViewProjectionMatrixInverse");
			compatLocs.normalMat        = glGetUniformLocation(objID, "spring_NormalMatrix");
			compatLocs.fogColor         = glGetUniformLocation(objID, "spring_Fog.color");
			compatLocs.fogDensity       = glGetUniformLocation(objID, "spring_Fog.density");
			compatLocs.fogStart         = glGetUniformLocation(objID, "spring_Fog.start");
			compatLocs.fogEnd           = glGetUniformLocation(objID, "spring_Fog.end");
			compatLocs.fogScale         = glGetUniformLocation(objID, "spring_Fog.scale");
		}

		// Skip only when the shader referenced none of the aliases at all.
		// NOTE: do not fold these with bitwise-or - -1 is all bits set, so
		// "a | b | ... == -1" is true as soon as ANY one of them is unused, which
		// silently skipped every shader that uses only some of the aliases.
		const bool anyAliasUsed =
			(compatLocs.modelView        != -1) ||
			(compatLocs.proj             != -1) ||
			(compatLocs.modelViewProj    != -1) ||
			(compatLocs.modelViewInv     != -1) ||
			(compatLocs.projInv          != -1) ||
			(compatLocs.modelViewProjInv != -1) ||
			(compatLocs.normalMat        != -1) ||
			(compatLocs.fogColor         != -1) ||
			(compatLocs.fogEnd           != -1);

		if (!anyAliasUsed)
			return;

		const CCamera* cam = CCameraHandler::GetActiveCamera();

		if (cam == nullptr)
			return;

		// The fixed-function modelview was view * model, and the engine draws
		// world geometry in world space with the camera view loaded, so model is
		// identity here. Call sites that pushed a per-object matrix need to set
		// these themselves; those are not converted yet.
		if (compatLocs.modelView        != -1) glUniformMatrix4fv(compatLocs.modelView       , 1, false, cam->GetViewMatrix());
		if (compatLocs.proj             != -1) glUniformMatrix4fv(compatLocs.proj            , 1, false, cam->GetProjectionMatrix());
		if (compatLocs.modelViewProj    != -1) glUniformMatrix4fv(compatLocs.modelViewProj   , 1, false, cam->GetViewProjectionMatrix());
		if (compatLocs.modelViewInv     != -1) glUniformMatrix4fv(compatLocs.modelViewInv    , 1, false, cam->GetViewMatrixInverse());
		if (compatLocs.projInv          != -1) glUniformMatrix4fv(compatLocs.projInv         , 1, false, cam->GetProjectionMatrixInverse());
		if (compatLocs.modelViewProjInv != -1) glUniformMatrix4fv(compatLocs.modelViewProjInv, 1, false, cam->GetViewProjectionMatrixInverse());

		// gl_Fog: mirror what ISky::SetupFog() used to push through glFog*.
		// Leaving it unset is not harmless - the shaders compute
		// fogFactor = (end - dist) * scale, so zeros clamp it to 0 and every
		// fragment comes out as the (black) fog colour.
		if (compatLocs.fogColor != -1 || compatLocs.fogEnd != -1) {
			const ISky* sky = ISky::GetSky().get();

			if (sky != nullptr) {
				const float farDist = cam->GetFarPlaneDist();

				float fogStart = farDist * sky->fogStart;
				float fogEnd   = farDist * sky->fogEnd;

				// with fog off the shaders still evaluate the same expression,
				// so push a range that clamps fogFactor to 1 instead
				if (!globalRendering->drawFog) {
					fogStart = farDist * 1.0e3f;
					fogEnd   = farDist * 2.0e3f;
				}

				const float fogScale = (fogEnd > fogStart) ? (1.0f / (fogEnd - fogStart)) : 0.0f;

				if (compatLocs.fogColor   != -1) glUniform4fv(compatLocs.fogColor, 1, &sky->fogColor.x);
				if (compatLocs.fogDensity != -1) glUniform1f(compatLocs.fogDensity, 1.0f);
				if (compatLocs.fogStart   != -1) glUniform1f(compatLocs.fogStart, fogStart);
				if (compatLocs.fogEnd     != -1) glUniform1f(compatLocs.fogEnd, fogEnd);
				if (compatLocs.fogScale   != -1) glUniform1f(compatLocs.fogScale, fogScale);
			}
		}

		if (compatLocs.normalMat != -1) {
			// the view matrix' upper-left 3x3 is orthonormal, so its
			// inverse-transpose - the normal matrix - is itself
			const CMatrix44f& v = cam->GetViewMatrix();
			const float n[9] = {v[0], v[1], v[2],  v[4], v[5], v[6],  v[8], v[9], v[10]};

			glUniformMatrix3fv(compatLocs.normalMat, 1, false, &n[0]);
		}
	}

	void GLSLProgramObject::SetCompatTransform(const CMatrix44f& mvp) {
		RECOIL_DETAILED_TRACY_ZONE;

		if (!globalRenderingInfo.glContextIsCore || objID == 0)
			return;

		SetCompatUniforms(); // resolves the locations if that has not happened yet

		// a screen-space or off-screen pass has no separate model/view split:
		// the whole transform is the projection the caller handed us
		static const CMatrix44f identity;

		if (compatLocs.modelViewProj != -1) glUniformMatrix4fv(compatLocs.modelViewProj, 1, false, mvp);
		if (compatLocs.modelView     != -1) glUniformMatrix4fv(compatLocs.modelView    , 1, false, identity);
		if (compatLocs.proj          != -1) glUniformMatrix4fv(compatLocs.proj         , 1, false, mvp);
	}

	void GLSLProgramObject::SetCompatModelMatrix(const CMatrix44f& model) {
		RECOIL_DETAILED_TRACY_ZONE;

		// remember it: a per-piece matrix is multiplied onto this one, the way
		// the fixed-function stack nested glMultMatrixf inside the object's push
		compatObjMatrix = model;
		UploadCompatModelMatrix(model);
	}

	void GLSLProgramObject::UploadCompatModelMatrix(const CMatrix44f& model) {
		RECOIL_DETAILED_TRACY_ZONE;

		if (!globalRenderingInfo.glContextIsCore || objID == 0)
			return;

		SetCompatUniforms(); // resolves the locations, and fills everything model-independent

		const CCamera* cam = CCameraHandler::GetActiveCamera();

		if (cam == nullptr)
			return;

		// the fixed-function modelview was view * model, and a call site that
		// pushed a per-object matrix was multiplying it onto the loaded view
		const CMatrix44f modelView = cam->GetViewMatrix() * model;

		if (compatLocs.modelView     != -1) glUniformMatrix4fv(compatLocs.modelView    , 1, false, modelView);
		if (compatLocs.modelViewProj != -1) glUniformMatrix4fv(compatLocs.modelViewProj, 1, false, cam->GetProjectionMatrix() * modelView);
		if (compatLocs.modelViewInv  != -1) glUniformMatrix4fv(compatLocs.modelViewInv , 1, false, modelView.Invert());

		if (compatLocs.normalMat != -1) {
			// unlike the view-only case this cannot be assumed orthonormal - a
			// model matrix may carry scale - so take the actual inverse-transpose
			const CMatrix44f n = modelView.InvertAffine().Transpose();
			const float m[9] = {n[0], n[1], n[2],  n[4], n[5], n[6],  n[8], n[9], n[10]};

			glUniformMatrix3fv(compatLocs.normalMat, 1, false, &m[0]);
		}
	}

	void GLSLProgramObject::EnableRaw() {
		RECOIL_DETAILED_TRACY_ZONE;
		glUseProgram(objID);
		boundProgram = this;
		SetCompatUniforms();
		IProgramObject::Enable();
	}
	void GLSLProgramObject::DisableRaw() {
		RECOIL_DETAILED_TRACY_ZONE;
		IProgramObject::Disable();

		if (boundProgram == this)
			boundProgram = nullptr;

		glUseProgram(0);
	}

	void GLSLProgramObject::Link() {
		// uniform locations do not survive a link, and a program may be linked
		// after something already enabled it once, so drop the cache here rather
		// than resolve it once and trust it forever
		compatLocs = CompatUniformLocs{};

		RECOIL_DETAILED_TRACY_ZONE;
		RecompileIfNeeded(false);
		assert(glIsProgram(objID));
	}

	bool GLSLProgramObject::Validate() {
		RECOIL_DETAILED_TRACY_ZONE;
		GLint validated = 0;

		// glValidateProgram reports whether the program can execute given the
		// CURRENT GL state, not whether it linked. A core profile requires a
		// bound vertex array object to draw, so calling this outside of a draw
		// setup fails with "No vertex array object bound" and would clobber the
		// link-time validity assigned below. Report nothing rather than turn a
		// state problem into a program one.
		if (globalRenderingInfo.glContextIsCore) {
			GLint boundVAO = 0;
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &boundVAO);

			if (boundVAO == 0)
				return valid;
		}

		glValidateProgram(objID);
		glGetProgramiv(objID, GL_VALIDATE_STATUS, &validated);
		valid = bool(validated);

		// append the validation-log
		log += glslGetLog(objID);

		const auto ReturnHelper = [this](const char* fn) -> bool {
			if (!valid && logReporting)
				LOG_L(L_ERROR, "[GLSL-PO::%s] program-object name: %s is not valid. Log:\n%s", fn, name.c_str(), log.c_str());
			return valid;
		};

	#ifdef _DEBUG
		{
			// check if there are unset uniforms left
			GLsizei numUniforms, maxUniformNameLength;
			glGetProgramiv(objID, GL_ACTIVE_UNIFORMS, &numUniforms);
			glGetProgramiv(objID, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxUniformNameLength);

			if (maxUniformNameLength <= 0)
				return ReturnHelper(__func__);

			std::string bufname(maxUniformNameLength, 0);
			for (uint32_t i = 0; i < numUniforms; ++i) {
				GLsizei nameLength = 0;
				GLint size = 0;
				GLint blockIdx = -1;
				GLenum type = 0;
				glGetActiveUniform(objID, i, maxUniformNameLength, &nameLength, &size, &type, &bufname[0]);
				bufname[nameLength] = 0;

				glGetActiveUniformsiv(objID, 1, &i, GL_UNIFORM_BLOCK_INDEX, &blockIdx);

				if (nameLength == 0)
					continue;

				if (strncmp(&bufname[0], "gl_", 3) == 0)
					continue;

				if (uniformStates.find(hashString(&bufname[0])) != uniformStates.end())
					continue;

				if (blockIdx >= 0) //ignore UBO
					continue;

				if (logReporting)
					LOG_L(L_WARNING, "[GLSL-PO::%s] program-object name: %s, unset uniform: %s", __func__, name.c_str(), &bufname[0]);
			}
		}
		{
			GLsizei numAttributes, maxNameLength = 0;
			glGetProgramiv(objID, GL_ACTIVE_ATTRIBUTES, &numAttributes);
			glGetProgramiv(objID, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxNameLength);

			if (maxNameLength <= 0)
				return ReturnHelper(__func__);

			std::string bufname(maxNameLength, 0);
			for (int i = 0; i < numAttributes; ++i) {
				GLsizei nameLength = 0;
				GLint size = 0;
				GLenum type = 0;
				glGetActiveAttrib(objID, i, maxNameLength, &nameLength, &size, &type, &bufname[0]);
				bufname[maxNameLength - 1] = 0;

				if (nameLength == 0)
					continue;

				if (strncmp(&bufname[0], "gl_", 3) == 0)
					continue;

				GLint loc = glGetAttribLocation(objID, &bufname[0]);

				if (loc < 0) {
					if (logReporting)
						LOG_L(L_WARNING, "[GLSL-PO::%s] program-object name: %s, atttrib: %s has undefined location", __func__, name.c_str(), &bufname[0]);
					valid = false;
				}
			}
		}
	#endif

		return ReturnHelper(__func__);
	}

	void GLSLProgramObject::Release() {
		RECOIL_DETAILED_TRACY_ZONE;
		IProgramObject::Release();
		glDeleteProgram(objID);
		shaderFlags.Clear();

		objID = 0;
		curSrcHash = 0;
	}

	void GLSLProgramObject::Reload(bool reloadFromDisk, bool validate) {
		compatLocs = CompatUniformLocs{};

		RECOIL_DETAILED_TRACY_ZONE;
		const unsigned int oldProgID = objID;
		const unsigned int oldSrcHash = curSrcHash;

		const bool oldValid = IsValid();
		valid = false;

		{
			// NOTE: this does not preserve the #version pragma
			for (IShaderObject*& so: shaderObjs) {
				so->SetDefinitions(shaderFlags.GetString());
			}

			log.clear();
		}

		// reload shader from disk if requested or necessary
		if ((reloadFromDisk || !oldValid || (oldProgID == 0))) {
			for (IShaderObject*& so: shaderObjs) {
				so->ReloadFromDisk();
			}
		}

		{
			// create shader source hash
			curSrcHash = shaderFlags.UpdateHash();

			for (const IShaderObject* so: shaderObjs) {
				curSrcHash ^= so->GetHash();
			}
		}

		// early-exit: empty program (TODO: delete it?)
		if (shaderObjs.empty())
			return;

		bool deleteOldShader = true;

		{
			objID = 0;

			// push old program to cache and pop new if available
			if (oldValid && configHandler->GetBool("UseShaderCache")) {
				CShaderHandler::ShaderCache& shadersCache = shaderHandler->GetShaderCache();
				deleteOldShader = !shadersCache.Push(oldSrcHash, oldProgID);
				objID = shadersCache.Find(curSrcHash);
			}
		}

		// recompile if not found in cache (id 0)
		if (objID == 0) {
			objID = glCreateProgram();

			bool shadersValid = true;
			for (IShaderObject*& so: shaderObjs) {
				assert(dynamic_cast<GLSLShaderObject*>(so));

				auto gso = static_cast<GLSLShaderObject*>(so);
				auto obj = gso->CompileShaderObject();

				if (obj->valid) {
					glAttachShader(objID, obj->id);
				} else {
					shadersValid = false;
				}
			}

			if (!shadersValid)
				return;

			for (const auto& [name, index] : attribLocations) {
				glBindAttribLocation(objID, index, name.c_str());
			}

			// pin the compatibility prelude's vertex inputs so that call sites
			// feeding geometry through generic attribute arrays know where to
			// put it. Binding a name the program does not use is harmless, and
			// an explicit layout qualifier in the shader still wins over this.
			if (globalRenderingInfo.glContextIsCore) {
				glBindAttribLocation(objID, COMPAT_ATTRIB_VERTEX   , "spring_Vertex");
				glBindAttribLocation(objID, COMPAT_ATTRIB_NORMAL   , "spring_Normal");
				glBindAttribLocation(objID, COMPAT_ATTRIB_COLOR    , "spring_Color");
				glBindAttribLocation(objID, COMPAT_ATTRIB_TEXCOORD0, "spring_MultiTexCoord0");
				glBindAttribLocation(objID, COMPAT_ATTRIB_TEXCOORD1, "spring_MultiTexCoord1");
			}

			for (const auto& [name, index] : outputLocations) {
				glBindFragDataLocation(objID, index, name.c_str());
			}

			glLinkProgram(objID);

			valid = glslIsValid(objID);
			log += glslGetLog(objID);

			if (!IsValid() && logReporting) {
				LOG_L(L_WARNING, "[GLSL-PO::%s] program-object name: %s, link-log:\n%s\n", __func__, name.c_str(), log.c_str());
			}

			#ifdef _DEBUG
			if (IsValid()) {
				for (const auto& [name, index] : attribLocations) {
					GLint indexOut = glGetAttribLocation(objID, name.c_str());
					if (indexOut == -1) {
						LOG_L(L_WARNING, "[GLSL-PO::%s] Attribute %s for program %u is unused(-1)", __func__, name.c_str(), objID);
					} 
					else if (indexOut != index) {
						LOG_L(L_ERROR, "[GLSL-PO::%s] Setting attribute %s to location %d(requested %d) for program %u", __func__, name.c_str(), indexOut, index, objID);
						assert(false);
					}
				}
			}
			#endif

		} else {
			valid = true;
		}

		{
			GLsizei numUniforms, maxUniformNameLength;
			glGetProgramiv(objID, GL_ACTIVE_UNIFORMS, &numUniforms);
			glGetProgramiv(objID, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxUniformNameLength);

			if (maxUniformNameLength > 0) {
				std::string bufname(maxUniformNameLength, 0);
				for (int i = 0; i < numUniforms; ++i) {
					GLsizei nameLength = 0;
					GLint size = 0;
					GLenum type = 0;
					glGetActiveUniform(objID, i, maxUniformNameLength, &nameLength, &size, &type, &bufname[0]);
					bufname[nameLength] = 0;

					if (nameLength == 0)
						continue;

					if (strncmp(bufname.c_str(), "gl_", 3) == 0)
						continue;

					// clear all uniform locations
					GetUniformState(bufname)->SetLocation(GL_INVALID_INDEX);
				}
			}
		}

		// FIXME: fails on ATI, see https://springrts.com/mantis/view.php?id=4715
		if (validate && (!globalRendering->haveAMD && !globalRendering->haveIntel))
			Validate();

		// copy full program state from old to new program (uniforms etc.)
		if (IsValid())
			GLSLCopyState(objID, oldValid ? oldProgID : 0, &uniformStates);

		// delete old program when not further used
		if (deleteOldShader)
			glDeleteProgram(oldProgID);
	}

	int GLSLProgramObject::GetUniformType(const int idx) {
		GLint size = 0;
		GLenum type = 0;
		// NB: idx can not be a *location* returned by glGetUniformLoc except on Nvidia
		glGetActiveUniform(objID, idx, 0, nullptr, &size, &type, nullptr);
		assert(size == 1); // arrays aren't handled yet
		return type;
	}

	int GLSLProgramObject::GetUniformLoc(const char* name) {
		return glGetUniformLocation(objID, name);
	}

	void GLSLProgramObject::SetUniformLocation(const std::string& name) {
		uniformLocs.push_back(hashString(name.c_str()));
		GetUniformLocation(name);
	}

	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0)                               { assert(IsBound()); if (uState->Set(v0            )) glUniform1i(uState->GetLocation(), v0             ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0)                               { assert(IsBound()); if (uState->Set(v0            )) glUniform1f(uState->GetLocation(), v0             ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1)                     { assert(IsBound()); if (uState->Set(v0, v1        )) glUniform2i(uState->GetLocation(), v0, v1         ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1)                     { assert(IsBound()); if (uState->Set(v0, v1        )) glUniform2f(uState->GetLocation(), v0, v1         ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1, int   v2)           { assert(IsBound()); if (uState->Set(v0, v1, v2    )) glUniform3i(uState->GetLocation(), v0, v1, v2     ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1, float v2)           { assert(IsBound()); if (uState->Set(v0, v1, v2    )) glUniform3f(uState->GetLocation(), v0, v1, v2     ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, int   v0, int   v1, int   v2, int   v3) { assert(IsBound()); if (uState->Set(v0, v1, v2, v3)) glUniform4i(uState->GetLocation(), v0, v1, v2, v3 ); }
	void GLSLProgramObject::SetUniform(UniformState* uState, float v0, float v1, float v2, float v3) { assert(IsBound()); if (uState->Set(v0, v1, v2, v3)) glUniform4f(uState->GetLocation(), v0, v1, v2, v3 ); }

	void GLSLProgramObject::SetUniform1v(UniformState* uState, GLsizei count, const int*   v) { assert(IsBound()); if (uState->Set2v(v)) glUniform1iv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform1v(UniformState* uState, GLsizei count, const float* v) { assert(IsBound()); if (uState->Set2v(v)) glUniform1fv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform2v(UniformState* uState, GLsizei count, const int*   v) { assert(IsBound()); if (uState->Set2v(v)) glUniform2iv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform2v(UniformState* uState, GLsizei count, const float* v) { assert(IsBound()); if (uState->Set2v(v)) glUniform2fv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform3v(UniformState* uState, GLsizei count, const int*   v) { assert(IsBound()); if (uState->Set3v(v)) glUniform3iv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform3v(UniformState* uState, GLsizei count, const float* v) { assert(IsBound()); if (uState->Set3v(v)) glUniform3fv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform4v(UniformState* uState, GLsizei count, const int*   v) { assert(IsBound()); if (uState->Set4v(v)) glUniform4iv(uState->GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform4v(UniformState* uState, GLsizei count, const float* v) { assert(IsBound()); if (uState->Set4v(v)) glUniform4fv(uState->GetLocation(), count, v); }

	void GLSLProgramObject::SetUniformMatrix2x2(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set2x2(v, transp)) glUniformMatrix2fv(uState->GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix3x3(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set3x3(v, transp)) glUniformMatrix3fv(uState->GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix4x4(UniformState* uState, bool transp, const float* v) { assert(IsBound()); if (uState->Set4x4(v, transp)) glUniformMatrix4fv(uState->GetLocation(), 1, transp, v); }

	void GLSLProgramObject::SetUniform1i(int idx, int   v0                              ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0            )) glUniform1i(it->second.GetLocation(), v0            ); }
	void GLSLProgramObject::SetUniform2i(int idx, int   v0, int   v1                    ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1        )) glUniform2i(it->second.GetLocation(), v0, v1        ); }
	void GLSLProgramObject::SetUniform3i(int idx, int   v0, int   v1, int   v2          ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2    )) glUniform3i(it->second.GetLocation(), v0, v1, v2    ); }
	void GLSLProgramObject::SetUniform4i(int idx, int   v0, int   v1, int   v2, int   v3) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2, v3)) glUniform4i(it->second.GetLocation(), v0, v1, v2, v3); }
	void GLSLProgramObject::SetUniform1f(int idx, float v0                              ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0            )) glUniform1f(it->second.GetLocation(), v0            ); }
	void GLSLProgramObject::SetUniform2f(int idx, float v0, float v1                    ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1        )) glUniform2f(it->second.GetLocation(), v0, v1        ); }
	void GLSLProgramObject::SetUniform3f(int idx, float v0, float v1, float v2          ) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2    )) glUniform3f(it->second.GetLocation(), v0, v1, v2    ); }
	void GLSLProgramObject::SetUniform4f(int idx, float v0, float v1, float v2, float v3) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set(v0, v1, v2, v3)) glUniform4f(it->second.GetLocation(), v0, v1, v2, v3); }

	void GLSLProgramObject::SetUniform2iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4iv(int idx, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4iv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform2fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2fv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform3fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3fv(it->second.GetLocation(), 1, v); }
	void GLSLProgramObject::SetUniform4fv(int idx, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4fv(it->second.GetLocation(), 1, v); }

	/// variants with count param
	void GLSLProgramObject::SetUniform1iv(int idx, const GLsizei count, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2iv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform2iv(int idx, const GLsizei count, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2iv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform3iv(int idx, const GLsizei count, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3iv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform4iv(int idx, const GLsizei count, const int*   v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4iv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform1fv(int idx, const GLsizei count, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2fv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform2fv(int idx, const GLsizei count, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2v(v)) glUniform2fv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform3fv(int idx, const GLsizei count, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3v(v)) glUniform3fv(it->second.GetLocation(), count, v); }
	void GLSLProgramObject::SetUniform4fv(int idx, const GLsizei count, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4v(v)) glUniform4fv(it->second.GetLocation(), count, v); }

	void GLSLProgramObject::SetUniformMatrix2fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set2x2(v, transp)) glUniformMatrix2fv(it->second.GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix3fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set3x3(v, transp)) glUniformMatrix3fv(it->second.GetLocation(), 1, transp, v); }
	void GLSLProgramObject::SetUniformMatrix4fv(int idx, bool transp, const float* v) { assert(IsBound()); auto it = uniformStates.find(uniformLocs[idx]); if (it != uniformStates.end() && it->second.Set4x4(v, transp)) glUniformMatrix4fv(it->second.GetLocation(), 1, transp, v); }
	ShaderEnabledToken::ShaderEnabledToken(IProgramObject* prog_)
		: prog(prog_)
	{
		if (prog)
			prog->Enable();
	}
	ShaderEnabledToken::~ShaderEnabledToken()
	{
		if (prog)
			prog->Disable();
	}
}
