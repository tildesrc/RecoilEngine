#include "ModelsDataUploader.h"

#include <limits>
#include <cassert>

#include "System/float4.h"
#include "System/Matrix44f.h"
#include "System/Log/ILog.h"
#include "Rendering/GL/myGL.h"
#include "System/SpringMath.h"
#include "System/TimeProfiler.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Objects/SolidObject.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureDefHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Rendering/GL/VBO.h"
#include "Rendering/Models/ModelsMemStorage.h"
#include "Rendering/Models/ModelsLock.h"
#include "Rendering/Units/UnitDrawer.h"
#include "Rendering/Features/FeatureDrawer.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Game/GlobalUnsynced.h"


////////////////////////////////////////////////////////////////////

namespace Impl {
	static constexpr uint32_t PERSISTENT_MAPPING_BUFFERING = 3u;

	template<
		typename DataType,
		typename SSBO,
		typename Uploader,
		typename MemStorage
	>
	void UpdateCommon(Uploader& uploader, std::unique_ptr<SSBO>& ssbo, MemStorage& memStorage, const char* className, const char* funcName)
	{
		auto& ul = memStorage.GetUpdateList();

		if (!ssbo || !ssbo->IsValid()) {
			ul.ResetNeedUpdateAll();
			return;
		}

		//resize handling
		const uint32_t elemCount = uploader.GetElemsCount();
		const uint32_t storageElemCount = memStorage.GetSize();
		if (storageElemCount > elemCount) {
			ssbo->UnbindBufferRange(uploader.GetBindingIdx());

			const uint32_t newElemCount = AlignUp(storageElemCount, uploader.GetElemCountIncr());
			LOG_L(L_DEBUG, "[%s::%s] sizing SSBO %s. New elements count = %u, elemCount = %u, storageElemCount = %u", className, funcName, "up", newElemCount, elemCount, storageElemCount);
			ssbo->Resize(newElemCount);

			if (!ssbo->IsValid()) {
				LOG_L(L_ERROR, "[%s::%s] Resizing of IStreamBuffer<%s> of type %d, failed. Falling back to SB_BUFFERSUBDATA.", className, __func__, spring::TypeToCStr<DataType>(), static_cast<int>(ssbo->GetBufferImplementation()));
				ssbo = nullptr;

				IStreamBufferConcept::StreamBufferCreationParams p;
				p.target = GL_SHADER_STORAGE_BUFFER;
				p.numElems = newElemCount;
				p.name = std::string(className);
				p.type = IStreamBufferConcept::Types::SB_BUFFERSUBDATA;
				p.resizeAble = true;
				p.coherent = false;
				p.numBuffers = 1;
				p.optimizeForStreaming = true;

				// must match p.numBuffers
				ul.SetTrueValue(1);

				ssbo = std::move(IStreamBuffer<DataType>::CreateInstance(p));
			}

			// ssbo->Resize() doesn't copy the data, force the update
			ul.SetNeedUpdateAll();
		}

		if (!ul.NeedUpdate())
			return;

		// may have been already unbound above, not a big deal
		ssbo->UnbindBufferRange(uploader.GetBindingIdx());

		// get the data
		const auto* clientPtr = memStorage.GetData().data();

		// iterate over contiguous regions of values that need update on the GPU
		for (auto itPair = ul.GetNext(); itPair.has_value(); itPair = ul.GetNext(itPair)) {
			auto [idxOffset, idxSize] = ul.GetOffsetAndSize(itPair.value());

			auto* mappedPtr = ssbo->Map(clientPtr, idxOffset, idxSize);

			if (!ssbo->HasClientPtr())
				std::copy(clientPtr + idxOffset, clientPtr + idxOffset + idxSize, mappedPtr);

			ul.DecrementUpdate(itPair.value());

			ssbo->Unmap();
		}

		ssbo->BindBufferRange(uploader.GetBindingIdx());
		ssbo->SwapBuffer();

		ul.CalcNeedUpdateAll();
	}

	template <
		typename DataType,
		typename SSBO,
		typename MemStorage
	>
	void InitCommon(std::unique_ptr<SSBO>& ssbo, MemStorage& memStorage, uint32_t bindingIdx, uint32_t elemCount0, uint32_t elemCountIncr, IStreamBufferConcept::Types type, bool coherent, uint32_t numBuffers, const char* className)
	{
		if (!globalRendering->haveGL4)
			return;

		assert(bindingIdx < -1u);

		IStreamBufferConcept::StreamBufferCreationParams p;
		p.target = GL_SHADER_STORAGE_BUFFER;
		p.numElems = elemCount0;
		p.name = std::string(className);
		p.type = type;
		p.resizeAble = true;
		p.coherent = coherent;
		p.numBuffers = numBuffers;
		p.optimizeForStreaming = true;

		auto& ul = memStorage.GetUpdateList();

		// must match p.numBuffers
		ul.SetTrueValue(static_cast<uint8_t>(numBuffers));

		ssbo = std::move(IStreamBuffer<DataType>::CreateInstance(p));
		if (!ssbo->IsValid()) {
			LOG_L(L_ERROR, "[%s::%s] Initialization of IStreamBuffer<%s> of type %d, failed. Falling back to SB_BUFFERSUBDATA.", className, __func__, spring::TypeToCStr<DataType>(), static_cast<int>(type));
			ssbo = nullptr;

			p.type = IStreamBufferConcept::Types::SB_BUFFERSUBDATA;
			p.coherent = false;
			p.numBuffers = 1;

			// must match p.numBuffers
			ul.SetTrueValue(1);

			ssbo = std::move(IStreamBuffer<DataType>::CreateInstance(p));
		}

		ssbo->BindBufferRange(bindingIdx);
	}

	template<typename SSBO>
	void KillCommon(std::unique_ptr<SSBO>& ssbo, uint32_t bindingIdx)
	{
		if (!globalRendering->haveGL4)
			return;

		ssbo->UnbindBufferRange(bindingIdx);
		ssbo = nullptr;
	}
}

////////////////////////////////////////////////////////////////////

TransformsUploader transformsUploader;
ModelUniformsUploader modelUniformsUploader;

////////////////////////////////////////////////////////////////////

void TransformsUploader::Init()
{
	const auto sbType = globalRendering->supportPersistentMapping
		? IStreamBufferConcept::Types::SB_PERSISTENTMAP
		: IStreamBufferConcept::Types::SB_BUFFERSUBDATA;

	Impl::InitCommon<MyDataType>(
		ssbo,
		transformsMemStorage,
		MATRIX_SSBO_BINDING_IDX, ELEM_COUNT0, ELEM_COUNTI,
		sbType, true, Impl::PERSISTENT_MAPPING_BUFFERING,
		className
	);
}

void TransformsUploader::Kill()
{
	Impl::KillCommon(ssbo, MATRIX_SSBO_BINDING_IDX);
}

void TransformsUploader::Update()
{
	if (!globalRendering->haveGL4)
		return;

	SCOPED_TIMER("TransformsUploader::Update");

	//auto lock = CModelsLock::GetScopedLock();

	Impl::UpdateCommon<MyDataType>(
		*this,
		ssbo,
		transformsMemStorage,
		className,
		__func__
	);
}

size_t TransformsUploader::GetElemOffset(const UnitDef* def) const
{
	if (def == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr UnitDef", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	return GetElemOffset(def->LoadModel());
}

size_t TransformsUploader::GetElemOffset(const FeatureDef* def) const
{
	if (def == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr FeatureDef", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	return GetElemOffset(def->LoadModel());
}

size_t TransformsUploader::GetElemOffset(const S3DModel* model) const
{
	if (model == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr S3DModel", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	return model->GetTransformAlloc().GetOffset(false);
}

size_t TransformsUploader::GetUnitDefElemOffset(int32_t unitDefID) const
{
	return GetElemOffset(unitDefHandler->GetUnitDefByID(unitDefID));
}

size_t TransformsUploader::GetFeatureDefElemOffset(int32_t featureDefID) const
{
	return GetElemOffset(featureDefHandler->GetFeatureDefByID(featureDefID));
}

size_t TransformsUploader::GetElemOffset(const CUnit* unit) const
{
	if (unit == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CUnit", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	if (size_t offset = CUnitDrawer::GetTransformMemAlloc(unit).GetOffset(false); offset != TransformsMemStorage::INVALID_INDEX) {
		return offset;
	}

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CUnit (id:%d)", className, __func__, unit->id);
	return TransformsMemStorage::INVALID_INDEX;
}

size_t TransformsUploader::GetElemOffset(const CFeature* feature) const
{
	if (feature == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CFeature", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	if (size_t offset = CFeatureDrawer::GetTransformMemAlloc(feature).GetOffset(false); offset != TransformsMemStorage::INVALID_INDEX) {
		return offset;
	}

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CFeature (id:%d)", className, __func__, feature->id);
	return TransformsMemStorage::INVALID_INDEX;
}

size_t TransformsUploader::GetElemOffset(const CProjectile* p) const
{
	if (p == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CProjectile", className, __func__);
		return TransformsMemStorage::INVALID_INDEX;
	}

	if (!p->synced) {
		LOG_L(L_ERROR, "[%s::%s] Supplied non-synced CProjectile (id:%d)", className, __func__, p->id);
		return TransformsMemStorage::INVALID_INDEX;
	}

	if (!p->weapon || !p->piece) {
		LOG_L(L_ERROR, "[%s::%s] Supplied non-weapon or non-piece CProjectile (id:%d)", className, __func__, p->id);
		return TransformsMemStorage::INVALID_INDEX;
	}
	/*
	if (size_t offset = p->GetMatAlloc().GetOffset(false); offset != TransformsMemStorage::INVALID_INDEX) {
		return offset;
	}
	*/

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CProjectile (id:%d)", className, __func__, p->id);
	return TransformsMemStorage::INVALID_INDEX;
}

size_t TransformsUploader::GetUnitElemOffset(int32_t unitID) const
{
	return GetElemOffset(unitHandler.GetUnit(unitID));
}

size_t TransformsUploader::GetFeatureElemOffset(int32_t featureID) const
{
	return GetElemOffset(featureHandler.GetFeature(featureID));
}

size_t TransformsUploader::GetProjectileElemOffset(int32_t syncedProjectileID) const
{
	return GetElemOffset(projectileHandler.GetProjectileBySyncedID(syncedProjectileID));
}

////////////////////////////////////////////////////////////////////

// SPRING_DBG_TEXBUF: feasibility probe for exposing this uploader's data as a
// buffer texture instead of an SSBO. A buffer texture is core since GL 3.1, so
// it is the one route to a large dynamically-indexed array on a 4.1 driver -
// the thing BAR's icon and health-bar shaders need and cannot have. Measures
// whether the pieces actually exist here rather than assuming they do.
static void ProbeTextureBufferFeasibility(uint32_t elemCount, uint32_t elemSize)
{
	// drain anything already queued, or this attributes someone else's error to
	// our query - which it did on the first run of this probe
	while (glGetError() != GL_NO_ERROR) {}

	GLint maxTexels = 0;
	glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
	const bool limitQueryOK = (glGetError() == GL_NO_ERROR);

	// is the SSBO target itself usable? it is a 4.3 enum, so this should fail -
	// a real implementation would have to create the buffer another way
	GLuint probe = 0;
	glGenBuffers(1, &probe);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, probe);
	const GLenum ssboTargetErr = glGetError();
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	glGetError();

	// now the route we would actually take
	const uint32_t byteSize = elemCount * elemSize;
	glBindBuffer(GL_TEXTURE_BUFFER, probe);
	glBufferData(GL_TEXTURE_BUFFER, byteSize, nullptr, GL_DYNAMIC_DRAW);
	const GLenum allocErr = glGetError();

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_BUFFER, tex);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, probe);
	const GLenum texBufErr = glGetError();

	const uint32_t texelsNeeded = byteSize / 16;

	LOG("[TEXBUF] GL_MAX_TEXTURE_BUFFER_SIZE = %d texels (query %s)", maxTexels, limitQueryOK ? "ok" : "FAILED");
	LOG("[TEXBUF] glBindBuffer(GL_SHADER_STORAGE_BUFFER) -> 0x%x %s", ssboTargetErr,
		(ssboTargetErr == GL_NO_ERROR) ? "(usable!)" : "(rejected, as expected on 4.1)");
	LOG("[TEXBUF] glBufferData(GL_TEXTURE_BUFFER, %u bytes) -> 0x%x", byteSize, allocErr);
	LOG("[TEXBUF] glTexBuffer(GL_RGBA32F) -> 0x%x %s", texBufErr,
		(texBufErr == GL_NO_ERROR) ? "WORKS" : "FAILED");
	LOG("[TEXBUF] need %u texels for %u elements of %u bytes -> headroom %.1fx; max elements = %u",
		texelsNeeded, elemCount, elemSize,
		texelsNeeded ? double(maxTexels) / double(texelsNeeded) : 0.0,
		(elemSize && maxTexels > 0) ? uint32_t((uint64_t(maxTexels) * 16) / elemSize) : 0u);

	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glDeleteTextures(1, &tex);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);
	glDeleteBuffers(1, &probe);
	glClearErrors("TEXBUF", __func__, false);
}

// The same per-object data as the SSBO, exposed as a GL_TEXTURE_BUFFER.
//
// Shaders that index an unbounded SSBO array cannot run below GL 4.3, but a
// buffer texture gives the same thing - a large array addressed by a computed
// integer - and has been core since 3.1. So where the SSBO path is unavailable
// this publishes the identical bytes through a sampler instead, and content
// reads them with texelFetch. See devtools/macos-port/SSBO-FALLBACK.md for the
// shader side.
//
// Only ModelUniformsUploader does this. TransformsUploader is triple-buffered,
// and pointing a texture at one segment of a ring needs glTexBufferRange, which
// is itself 4.3.
void ModelUniformsUploader::InitTexBuffer()
{
	const uint32_t elems = std::max(ELEM_COUNT0, static_cast<uint32_t>(modelUniformsStorage.GetSize()));

	glGenBuffers(1, &texBufVBO);
	// NOT GL_SHADER_STORAGE_BUFFER: that enum is 4.3 and raises GL_INVALID_ENUM
	// on the very drivers this path exists for
	glBindBuffer(GL_TEXTURE_BUFFER, texBufVBO);
	glBufferData(GL_TEXTURE_BUFFER, elems * sizeof(MyDataType), nullptr, GL_DYNAMIC_DRAW);

	glGenTextures(1, &texBufTex);
	glBindTexture(GL_TEXTURE_BUFFER, texBufTex);
	// RGBA32F: ModelUniformData is 16-byte aligned and a multiple of 16 in size,
	// so one element is an exact whole number of texels (8 at 128 bytes)
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, texBufVBO);

	glBindTexture(GL_TEXTURE_BUFFER, 0);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);

	texBufElems = elems;

	LOG("[%s::%s] buffer-texture fallback active: tex=%u vbo=%u elems=%u (%u texels/elem)",
		className, __func__, texBufTex, texBufVBO, texBufElems, uint32_t(sizeof(MyDataType) / 16));

	glClearErrors(className, __func__, false);
}

void ModelUniformsUploader::UpdateTexBuffer()
{
	if (texBufVBO == 0u)
		return;

	auto& ul = modelUniformsStorage.GetUpdateList();
	const uint32_t storageElems = static_cast<uint32_t>(modelUniformsStorage.GetSize());

	if (storageElems > texBufElems) {
		const uint32_t newElems = AlignUp(storageElems, ELEM_COUNTI);

		glBindBuffer(GL_TEXTURE_BUFFER, texBufVBO);
		glBufferData(GL_TEXTURE_BUFFER, newElems * sizeof(MyDataType), nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_TEXTURE_BUFFER, 0);

		// glBufferData discards the old contents, so everything is stale now
		texBufElems = newElems;
		ul.SetNeedUpdateAll();
	}

	if (!ul.NeedUpdate())
		return;

	const auto* clientPtr = modelUniformsStorage.GetData().data();

	glBindBuffer(GL_TEXTURE_BUFFER, texBufVBO);

	// upload only the regions that changed, as the SSBO path does
	for (auto itPair = ul.GetNext(); itPair.has_value(); itPair = ul.GetNext(itPair)) {
		auto [idxOffset, idxSize] = ul.GetOffsetAndSize(itPair.value());

		glBufferSubData(GL_TEXTURE_BUFFER,
			idxOffset * sizeof(MyDataType),
			idxSize * sizeof(MyDataType),
			clientPtr + idxOffset);

		ul.DecrementUpdate(itPair.value());
	}

	glBindBuffer(GL_TEXTURE_BUFFER, 0);

	ul.CalcNeedUpdateAll();

	// SPRING_DBG_TEXBUF: read one element back off the GPU and compare it with
	// the CPU-side value it came from. Proves the whole path end to end - a
	// buffer that exists and a texture that binds say nothing about whether the
	// bytes arrived.
	if (getenv("SPRING_DBG_TEXBUF") != nullptr) {
		static int reported = 0;

		if (reported < 3) {
			const auto& data = modelUniformsStorage.GetData();

			for (size_t i = 0; i < std::min<size_t>(data.size(), texBufElems); i++) {
				if (data[i].health <= 0.0f || data[i].maxHealth <= 0.0f)
					continue;

				MyDataType gpu = {};
				glBindBuffer(GL_TEXTURE_BUFFER, texBufVBO);
				glGetBufferSubData(GL_TEXTURE_BUFFER, i * sizeof(MyDataType), sizeof(MyDataType), &gpu);
				glBindBuffer(GL_TEXTURE_BUFFER, 0);

				const bool match = (gpu.health == data[i].health && gpu.maxHealth == data[i].maxHealth && gpu.id == data[i].id);

				LOG("[TEXBUF] elem %zu: cpu(id=%u hp=%.1f/%.1f) gpu(id=%u hp=%.1f/%.1f) -> %s",
					i, data[i].id, data[i].health, data[i].maxHealth,
					gpu.id, gpu.health, gpu.maxHealth, match ? "MATCH" : "MISMATCH");

				reported++;
				break;
			}

			glClearErrors(className, __func__, false);
		}
	}
}

void ModelUniformsUploader::KillTexBuffer()
{
	if (texBufTex != 0u) { glDeleteTextures(1, &texBufTex); texBufTex = 0u; }
	if (texBufVBO != 0u) { glDeleteBuffers (1, &texBufVBO); texBufVBO = 0u; }

	texBufElems = 0u;
}

void ModelUniformsUploader::Init()
{
	if (getenv("SPRING_DBG_TEXBUF") != nullptr)
		ProbeTextureBufferFeasibility(ELEM_COUNT0, sizeof(MyDataType));

	if (!globalRendering->haveGL4) {
		// the SSBO path leaves ssbo null here, so nothing below runs and this
		// cannot interfere with it
		InitTexBuffer();
		return;
	}

	Impl::InitCommon<MyDataType>(
		ssbo,
		modelUniformsStorage,
		MATUNI_SSBO_BINDING_IDX, ELEM_COUNT0, ELEM_COUNTI,
		IStreamBufferConcept::Types::SB_BUFFERSUBDATA, true, 1,
		className
	);
}

void ModelUniformsUploader::Kill()
{
	KillTexBuffer();
	Impl::KillCommon(ssbo, MATUNI_SSBO_BINDING_IDX);
}

void ModelUniformsUploader::Update()
{
	SCOPED_TIMER("ModelUniformsUploader::Update");

	if (texBufVBO != 0u) {
		UpdateTexBuffer();
		return;
	}

	Impl::UpdateCommon<MyDataType>(*this, ssbo, modelUniformsStorage, className, __func__);
}

size_t ModelUniformsUploader::GetElemOffset(const UnitDef* def) const
{
	assert(false);
	LOG_L(L_ERROR, "[%s::%s] Invalid call", className, __func__);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetElemOffset(const FeatureDef* def) const
{
	assert(false);
	LOG_L(L_ERROR, "[%s::%s] Invalid call", className, __func__);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetElemOffset(const S3DModel* model) const
{
	assert(false);
	LOG_L(L_ERROR, "[%s::%s] Invalid call", className, __func__);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetUnitDefElemOffset(int32_t unitDefID) const
{
	return GetElemOffset(unitDefHandler->GetUnitDefByID(unitDefID));
}

size_t ModelUniformsUploader::GetFeatureDefElemOffset(int32_t featureDefID) const
{
	return GetElemOffset(featureDefHandler->GetFeatureDefByID(featureDefID));
}

size_t ModelUniformsUploader::GetElemOffset(const CUnit* unit) const
{
	if (unit == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CUnit", className, __func__);
		return ModelUniformsStorage::INVALID_INDEX;
	}

	if (size_t offset = modelUniformsStorage.GetObjOffset(unit); offset != size_t(-1)) {
		return offset;
	}

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CUnit (id:%d)", className, __func__, unit->id);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetElemOffset(const CFeature* feature) const
{
	if (feature == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CFeature", className, __func__);
		return ModelUniformsStorage::INVALID_INDEX;
	}

	if (size_t offset = modelUniformsStorage.GetObjOffset(feature); offset != size_t(-1)) {
		return offset;
	}

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CFeature (id:%d)", className, __func__, feature->id);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetElemOffset(const CProjectile* p) const
{
	if (p == nullptr) {
		LOG_L(L_ERROR, "[%s::%s] Supplied nullptr CProjectile", className, __func__);
		return ModelUniformsStorage::INVALID_INDEX;
	}

	if (size_t offset = modelUniformsStorage.GetObjOffset(p); offset != size_t(-1)) {
		return offset;
	}

	LOG_L(L_ERROR, "[%s::%s] Supplied invalid CProjectile (id:%d)", className, __func__, p->id);
	return ModelUniformsStorage::INVALID_INDEX;
}

size_t ModelUniformsUploader::GetUnitElemOffset(int32_t unitID) const
{
	return GetElemOffset(unitHandler.GetUnit(unitID));
}

size_t ModelUniformsUploader::GetFeatureElemOffset(int32_t featureID) const
{
	return GetElemOffset(featureHandler.GetFeature(featureID));
}

size_t ModelUniformsUploader::GetProjectileElemOffset(int32_t syncedProjectileID) const
{
	return GetElemOffset(projectileHandler.GetProjectileBySyncedID(syncedProjectileID));
}
