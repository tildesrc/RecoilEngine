#pragma once

#include <string>
#include <string_view>
#include <stddef.h>
#include <memory>

#include "System/Transform.hpp"
#include "System/TypeToStr.h"
#include "Rendering/GL/StreamBuffer.h"
#include "Rendering/Models/ModelsMemStorageDefs.h"


class CUnit;
class CFeature;
class CProjectile;
struct UnitDef;
struct FeatureDef;
struct S3DModel;

// A GL_TEXTURE_BUFFER mirror of an uploader's data, for drivers where the SSBO
// path cannot run. Buffer textures are core since GL 3.1 and give a shader the
// same thing an SSBO does here: a large array addressed by a computed integer.
// See devtools/macos-port/SSBO-FALLBACK.md.
namespace Impl {
	struct TexBuffer {
		uint32_t vbo = 0u;
		uint32_t tex = 0u;
		uint32_t elems = 0u;
		uint32_t unit = 0u;
	};
}

class TransformsUploader {
public:
	using MyClassName = TransformsUploader;
	using MyDataType = Transform;
public:
	void Init();
	void Kill();
	void Update();
public:
	uint32_t GetElemsCount() const { return ssbo->GetByteSize() / sizeof(MyDataType); }
	constexpr uint32_t GetBindingIdx() const { return MATRIX_SSBO_BINDING_IDX; }
	constexpr uint32_t GetElemCountIncr() const { return ELEM_COUNTI; }

	// see ModelUniformsUploader; zero when the SSBO path is in use
	uint32_t GetTexBufferID() const { return texBuf.tex; }
	uint32_t GetTexBufferElems() const { return texBuf.elems; }

	static constexpr uint32_t TEXBUF_UNIT = 13u;
public:
	// Defs
	size_t GetElemOffset(const UnitDef* def) const;
	size_t GetElemOffset(const FeatureDef* def) const;
	size_t GetElemOffset(const S3DModel* model) const;
	size_t GetUnitDefElemOffset(int32_t unitDefID) const;
	size_t GetFeatureDefElemOffset(int32_t featureDefID) const;

	// Objs
	size_t GetElemOffset(const CUnit* unit) const;
	size_t GetElemOffset(const CFeature* feature) const;
	size_t GetElemOffset(const CProjectile* proj) const;
	size_t GetUnitElemOffset(int32_t unitID) const;
	size_t GetFeatureElemOffset(int32_t featureID) const;
	size_t GetProjectileElemOffset(int32_t syncedProjectileID) const;
private:
	std::unique_ptr<IStreamBuffer<MyDataType>> ssbo;
	Impl::TexBuffer texBuf;
private:
	static constexpr const char* className = spring::TypeToCStr<MyClassName>();
	static constexpr uint32_t MATRIX_SSBO_BINDING_IDX = 0;
	static constexpr uint32_t ELEM_COUNT0 = 1u << 16;
	static constexpr uint32_t ELEM_COUNTI = 1u << 14;
};

class ModelUniformsUploader {
public:
	using MyClassName = ModelUniformsUploader;
	using MyDataType = ModelUniformData;
public:
	void Init();
	void Kill();
	void Update();
public:
	uint32_t GetElemsCount() const { return ssbo->GetByteSize() / sizeof(MyDataType); }
	constexpr uint32_t GetBindingIdx() const { return MATUNI_SSBO_BINDING_IDX; }
	constexpr uint32_t GetElemCountIncr() const { return ELEM_COUNTI; }

	// Buffer-texture mirror of the same data, for drivers without SSBO support.
	// Zero when the SSBO path is in use, so a caller can tell which to expect.
	// See devtools/macos-port/SSBO-FALLBACK.md.
	uint32_t GetTexBufferID() const { return texBuf.tex; }
	uint32_t GetTexBufferElems() const { return texBuf.elems; }

	// reserved texture unit; shaders name it with layout(binding = N)
	static constexpr uint32_t TEXBUF_UNIT = 14u;
public:
	// Defs
	size_t GetElemOffset(const UnitDef* def) const;
	size_t GetElemOffset(const FeatureDef* def) const;
	size_t GetElemOffset(const S3DModel* model) const;
	size_t GetUnitDefElemOffset(int32_t unitDefID) const;
	size_t GetFeatureDefElemOffset(int32_t featureDefID) const;

	// Objs
	size_t GetElemOffset(const CUnit* unit) const;
	size_t GetElemOffset(const CFeature* feature) const;
	size_t GetElemOffset(const CProjectile* proj) const;
	size_t GetUnitElemOffset(int32_t unitID) const;
	size_t GetFeatureElemOffset(int32_t featureID) const;
	size_t GetProjectileElemOffset(int32_t syncedProjectileID) const;
private:
	void InitTexBuffer();
	void UpdateTexBuffer();
	void KillTexBuffer();
private:
	std::unique_ptr<IStreamBuffer<MyDataType>> ssbo;

	// GL_TEXTURE_BUFFER fallback, used instead of the SSBO where that is absent
	Impl::TexBuffer texBuf;
private:
	static constexpr const char* className = spring::TypeToCStr<MyClassName>();
	static constexpr uint32_t MATUNI_SSBO_BINDING_IDX = 1;
	static constexpr uint32_t ELEM_COUNT0 = 1u << 12;
	static constexpr uint32_t ELEM_COUNTI = 1u << 11;
};

extern TransformsUploader transformsUploader;
extern ModelUniformsUploader modelUniformsUploader;