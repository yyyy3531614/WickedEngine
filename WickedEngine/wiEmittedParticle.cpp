#include "wiEmittedParticle.h"
#include "wiMath.h"
#include "wiLoader.h"
#include "wiRenderer.h"
#include "wiResourceManager.h"
#include "wiFrustum.h"
#include "wiRandom.h"
#include "ResourceMapping.h"
#include "wiArchive.h"
#include "wiTextureHelper.h"

using namespace std;
using namespace wiGraphicsTypes;

VertexShader  *wiEmittedParticle::vertexShader = nullptr;
PixelShader   *wiEmittedParticle::pixelShader[PARTICLESHADERTYPE_COUNT] = {};
ComputeShader   *wiEmittedParticle::kickoffUpdateCS, *wiEmittedParticle::emitCS = nullptr, *wiEmittedParticle::simulateCS = nullptr,
				 *wiEmittedParticle::simulateCS_SORTING = nullptr, *wiEmittedParticle::simulateCS_DEPTHCOLLISIONS = nullptr, *wiEmittedParticle::simulateCS_SORTING_DEPTHCOLLISIONS = nullptr;
ComputeShader		*wiEmittedParticle::kickoffSortCS = nullptr, *wiEmittedParticle::sortCS = nullptr, *wiEmittedParticle::sortInnerCS = nullptr, *wiEmittedParticle::sortStepCS = nullptr;
GPUBuffer		*wiEmittedParticle::sortCB = nullptr;
BlendState		wiEmittedParticle::blendStates[BLENDMODE_COUNT];
RasterizerState		wiEmittedParticle::rasterizerState, wiEmittedParticle::wireFrameRS;
DepthStencilState	wiEmittedParticle::depthStencilState;
GraphicsPSO wiEmittedParticle::PSO[BLENDMODE_COUNT][PARTICLESHADERTYPE_COUNT];
GraphicsPSO wiEmittedParticle::PSO_wire;
ComputePSO wiEmittedParticle::CPSO_kickoffUpdate, wiEmittedParticle::CPSO_emit, wiEmittedParticle::CPSO_simulate, 
	wiEmittedParticle::CPSO_simulate_SORTING, wiEmittedParticle::CPSO_simulate_DEPTHCOLLISIONS, wiEmittedParticle::CPSO_simulate_SORTING_DEPTHCOLLISIONS;
ComputePSO wiEmittedParticle::CPSO_kickoffSort, wiEmittedParticle::CPSO_sort, wiEmittedParticle::CPSO_sortInner, 
	wiEmittedParticle::CPSO_sortStep;

wiEmittedParticle::wiEmittedParticle()
{
	name = "";
	object = nullptr;
	materialName = "";
	material = nullptr;

	size = 1;
	random_factor = 0;
	normal_factor = 1;

	count = 1;
	life = 60;
	random_life = 0;
	emit = 0;

	scaleX = 1;
	scaleY = 1;
	rotation = 0;

	motionBlurAmount = 0.0f;

	SAFE_INIT(particleBuffer);
	SAFE_INIT(aliveList[0]);
	SAFE_INIT(aliveList[1]);
	SAFE_INIT(deadList);
	SAFE_INIT(distanceBuffer);
	SAFE_INIT(counterBuffer);
	SAFE_INIT(indirectBuffers);
	SAFE_INIT(constantBuffer);
	SAFE_INIT(debugDataReadbackBuffer);

	SetMaxParticleCount(10000);
}
wiEmittedParticle::wiEmittedParticle(const std::string& newName, const std::string& newMat, Object* newObject, float newSize, float newRandomFac, float newNormalFac
		,float newCount, float newLife, float newRandLife, float newScaleX, float newScaleY, float newRot)
{
	name=newName;
	object=newObject;
	materialName = newMat;
	for (MeshSubset& subset : object->mesh->subsets)
	{
		if (!newMat.compare(subset.material->name)) {
			material = subset.material;
			break;
		}
	}

	size=newSize;
	random_factor=newRandomFac;
	normal_factor=newNormalFac;

	count=newCount;
	life=newLife;
	random_life=newRandLife;
	emit=0;
	
	scaleX=newScaleX;
	scaleY=newScaleY;
	rotation = newRot;

	motionBlurAmount = 0.0f;


	SAFE_INIT(particleBuffer);
	SAFE_INIT(aliveList[0]);
	SAFE_INIT(aliveList[1]);
	SAFE_INIT(deadList);
	SAFE_INIT(distanceBuffer);
	SAFE_INIT(counterBuffer);
	SAFE_INIT(indirectBuffers);
	SAFE_INIT(constantBuffer);
	SAFE_INIT(debugDataReadbackBuffer);

	SetMaxParticleCount(10000);
}
wiEmittedParticle::wiEmittedParticle(const wiEmittedParticle& other)
{
	name = other.name + "0";
	object = other.object;
	materialName = other.materialName;
	material = other.material;
	size = other.size;
	random_factor = other.random_factor;
	normal_factor = other.normal_factor;
	count = other.count;
	life = other.life;
	random_life = other.random_life;
	emit = 0;
	scaleX = other.scaleX;
	scaleY = other.scaleY;
	rotation = other.rotation;
	motionBlurAmount = other.motionBlurAmount;


	SAFE_INIT(particleBuffer);
	SAFE_INIT(aliveList[0]);
	SAFE_INIT(aliveList[1]);
	SAFE_INIT(deadList);
	SAFE_INIT(distanceBuffer);
	SAFE_INIT(counterBuffer);
	SAFE_INIT(indirectBuffers);
	SAFE_INIT(constantBuffer);
	SAFE_INIT(debugDataReadbackBuffer);

	SetMaxParticleCount(other.GetMaxParticleCount());
}

void wiEmittedParticle::SetMaxParticleCount(uint32_t value)
{
	buffersUpToDate = false;
	MAX_PARTICLES = value;
}

void wiEmittedParticle::CreateSelfBuffers()
{
	if (buffersUpToDate)
	{
		return;
	}
	buffersUpToDate = true;

	SAFE_DELETE(particleBuffer);
	SAFE_DELETE(aliveList[0]);
	SAFE_DELETE(aliveList[1]);
	SAFE_DELETE(deadList);
	SAFE_DELETE(distanceBuffer);
	SAFE_DELETE(counterBuffer);
	SAFE_DELETE(indirectBuffers);
	SAFE_DELETE(constantBuffer);
	SAFE_DELETE(debugDataReadbackBuffer);

	particleBuffer = new GPUBuffer;
	aliveList[0] = new GPUBuffer;
	aliveList[1] = new GPUBuffer;
	deadList = new GPUBuffer;
	distanceBuffer = new GPUBuffer;
	counterBuffer = new GPUBuffer;
	indirectBuffers = new GPUBuffer;
	constantBuffer = new GPUBuffer;
	debugDataReadbackBuffer = new GPUBuffer;

	GPUBufferDesc bd;
	bd.Usage = USAGE_DEFAULT;
	bd.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
	bd.CPUAccessFlags = 0;
	bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
	SubresourceData data;


	bd.ByteWidth = sizeof(Particle) * MAX_PARTICLES;
	bd.StructureByteStride = sizeof(Particle);
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, particleBuffer);

	bd.ByteWidth = sizeof(uint32_t) * MAX_PARTICLES;
	bd.StructureByteStride = sizeof(uint32_t);
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, aliveList[0]);
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, aliveList[1]);

	uint32_t* indices = new uint32_t[MAX_PARTICLES];
	for (uint32_t i = 0; i < MAX_PARTICLES; ++i)
	{
		indices[i] = i;
	}
	data.pSysMem = indices;
	wiRenderer::GetDevice()->CreateBuffer(&bd, &data, deadList);
	SAFE_DELETE_ARRAY(indices);
	data.pSysMem = nullptr;

	float* distances = new float[MAX_PARTICLES];
	for (uint32_t i = 0; i < MAX_PARTICLES; ++i)
	{
		distances[i] = FLOAT32_MAX;
	}
	data.pSysMem = distances;
	wiRenderer::GetDevice()->CreateBuffer(&bd, &data, distanceBuffer);
	SAFE_DELETE_ARRAY(distances);
	data.pSysMem = nullptr;


	ParticleCounters counters;
	counters.aliveCount = 0;
	counters.deadCount = MAX_PARTICLES;
	counters.realEmitCount = 0;

	data.pSysMem = &counters;
	bd.ByteWidth = sizeof(counters);
	bd.StructureByteStride = sizeof(counters);
	wiRenderer::GetDevice()->CreateBuffer(&bd, &data, counterBuffer);
	data.pSysMem = nullptr;


	bd.BindFlags = BIND_UNORDERED_ACCESS;
	bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | RESOURCE_MISC_DRAWINDIRECT_ARGS;
	bd.ByteWidth = 
		sizeof(wiGraphicsTypes::IndirectDispatchArgs) + 
		sizeof(wiGraphicsTypes::IndirectDispatchArgs) + 
		sizeof(wiGraphicsTypes::IndirectDrawArgsInstanced) + 
		sizeof(wiGraphicsTypes::IndirectDispatchArgs);
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, indirectBuffers);


	bd.Usage = USAGE_DYNAMIC;
	bd.ByteWidth = sizeof(EmittedParticleCB);
	bd.BindFlags = BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = CPU_ACCESS_WRITE;
	bd.MiscFlags = 0;
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, constantBuffer);

	{
		GPUBufferDesc debugBufDesc = counterBuffer->GetDesc();
		debugBufDesc.Usage = USAGE_STAGING;
		debugBufDesc.CPUAccessFlags = CPU_ACCESS_READ;
		debugBufDesc.BindFlags = 0;
		wiRenderer::GetDevice()->CreateBuffer(&debugBufDesc, nullptr, debugDataReadbackBuffer);
	}
}

uint32_t wiEmittedParticle::GetMemorySizeInBytes() const
{
	if (particleBuffer == nullptr)
		return 0;

	uint32_t retVal = 0;

	retVal += particleBuffer->GetDesc().ByteWidth;
	retVal += aliveList[0]->GetDesc().ByteWidth;
	retVal += aliveList[1]->GetDesc().ByteWidth;
	retVal += deadList->GetDesc().ByteWidth;
	retVal += distanceBuffer->GetDesc().ByteWidth;
	retVal += counterBuffer->GetDesc().ByteWidth;
	retVal += indirectBuffers->GetDesc().ByteWidth;
	retVal += constantBuffer->GetDesc().ByteWidth;

	return retVal;
}

XMFLOAT3 wiEmittedParticle::GetPosition() const
{
	return object == nullptr ? XMFLOAT3(0, 0, 0) : object->translation;
}

void wiEmittedParticle::Update(float dt)
{
	emit += (float)count*dt;
}
void wiEmittedParticle::Burst(float num)
{
	emit += num;
}


void wiEmittedParticle::UpdateRenderData(GRAPHICSTHREAD threadID)
{
	CreateSelfBuffers();

	GraphicsDevice* device = wiRenderer::GetDevice();
	device->EventBegin("UpdateEmittedParticles", threadID);

	EmittedParticleCB cb;
	cb.xEmitterWorld = object->world;
	cb.xEmitCount = (UINT)emit;
	cb.xEmitterMeshIndexCount = (UINT)object->mesh->indices.size();
	cb.xEmitterMeshVertexPositionStride = sizeof(Mesh::Vertex_POS);
	cb.xEmitterRandomness = wiRandom::getRandom(0, 1000) * 0.001f;
	cb.xParticleLifeSpan = life / 60.0f;
	cb.xParticleLifeSpanRandomness = random_life;
	cb.xParticleNormalFactor = normal_factor;
	cb.xParticleRandomFactor = random_factor;
	cb.xParticleScaling = scaleX;
	cb.xParticleSize = size;
	cb.xParticleMotionBlurAmount = motionBlurAmount;
	cb.xParticleRotation = rotation * XM_PI * 60;
	cb.xParticleColor = wiMath::CompressColor(XMFLOAT4(material->baseColor.x, material->baseColor.y, material->baseColor.z, 1));
	cb.xEmitterOpacity = material->alpha;

	device->UpdateBuffer(constantBuffer, &cb, threadID);
	device->BindConstantBuffer(CS, constantBuffer, CB_GETBINDSLOT(EmittedParticleCB), threadID);

	GPUResource* uavs[] = {
		particleBuffer,
		aliveList[0], // CURRENT alivelist
		aliveList[1], // NEW alivelist
		deadList,
		counterBuffer,
		indirectBuffers,
		distanceBuffer,
	};
	device->BindUnorderedAccessResourcesCS(uavs, 0, ARRAYSIZE(uavs), threadID);
	
	GPUResource* resources[] = {
		wiTextureHelper::getInstance()->getRandom64x64(),
		object->mesh->indexBuffer,
		object->mesh->vertexBuffer_POS,
	};
	device->BindResources(CS, resources, TEXSLOT_ONDEMAND0, ARRAYSIZE(resources), threadID);

	GPUResource* indres[] = {
		indirectBuffers
	};
	device->TransitionBarrier(indres, 1, RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS, threadID);

	// kick off updating, set up state
	device->BindComputePSO(&CPSO_kickoffUpdate, threadID);
	device->Dispatch(1, 1, 1, threadID);
	device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

	device->TransitionBarrier(indres, 1, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT, threadID);

	// emit the required amount if there are free slots in dead list
	device->BindComputePSO(&CPSO_emit, threadID);
	device->DispatchIndirect(indirectBuffers, ARGUMENTBUFFER_OFFSET_DISPATCHEMIT, threadID);
	device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

	// update CURRENT alive list, write NEW alive list
	if (SORTING)
	{
		if (DEPTHCOLLISIONS)
		{
			device->BindComputePSO(&CPSO_simulate_SORTING_DEPTHCOLLISIONS, threadID);
		}
		else
		{
			device->BindComputePSO(&CPSO_simulate_SORTING, threadID);
		}
	}
	else
	{
		if (DEPTHCOLLISIONS)
		{
			device->BindComputePSO(&CPSO_simulate_DEPTHCOLLISIONS, threadID);
		}
		else
		{
			device->BindComputePSO(&CPSO_simulate, threadID);
		}
	}
	device->DispatchIndirect(indirectBuffers, ARGUMENTBUFFER_OFFSET_DISPATCHSIMULATION, threadID);
	device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

	device->EventEnd(threadID);

	if (SORTING)
	{
		device->EventBegin("SortEmittedParticles", threadID);

		// initialize sorting arguments:
		device->BindComputePSO(&CPSO_kickoffSort, threadID);
		device->Dispatch(1, 1, 1, threadID);
		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

		// initial sorting:
		bool bDone = true;

		// calculate how many threads we'll require:
		//   we'll sort 512 elements per CU (threadgroupsize 256)
		//     maybe need to optimize this or make it changeable during init
		//     TGS=256 is a good intermediate value

		unsigned int numThreadGroups = ((MAX_PARTICLES - 1) >> 9) + 1;

		if (numThreadGroups>1) bDone = false;

		// sort all buffers of size 512 (and presort bigger ones)
		device->BindComputePSO(&CPSO_sort, threadID);
		device->DispatchIndirect(indirectBuffers, ARGUMENTBUFFER_OFFSET_DISPATCHSORT, threadID);
		device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);

		int presorted = 512;
		while (!bDone)
		{
			bDone = true;
			device->BindComputePSO(&CPSO_sortStep, threadID);

			// prepare thread group description data
			unsigned int numThreadGroups = 0;

			if (MAX_PARTICLES > (uint32_t)presorted)
			{
				if (MAX_PARTICLES>(uint32_t)presorted * 2)
					bDone = false;

				unsigned int pow2 = presorted;
				while (pow2<MAX_PARTICLES)
					pow2 *= 2;
				numThreadGroups = pow2 >> 9;
			}

			unsigned int nMergeSize = presorted * 2;
			for (unsigned int nMergeSubSize = nMergeSize >> 1; nMergeSubSize>256; nMergeSubSize = nMergeSubSize >> 1)
			{
				SortConstants sc;
				sc.job_params.x = nMergeSubSize;
				if (nMergeSubSize == nMergeSize >> 1)
				{
					sc.job_params.y = (2 * nMergeSubSize - 1);
					sc.job_params.z = -1;
				}
				else
				{
					sc.job_params.y = nMergeSubSize;
					sc.job_params.z = 1;
				}
				sc.job_params.w = 0;

				device->UpdateBuffer(sortCB, &sc, threadID);
				
				device->Dispatch(numThreadGroups, 1, 1, threadID);
				device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);
			}

			device->BindComputePSO(&CPSO_sortInner, threadID);
			device->Dispatch(numThreadGroups, 1, 1, threadID);
			device->UAVBarrier(uavs, ARRAYSIZE(uavs), threadID);


			presorted *= 2;
		}

		device->EventEnd(threadID);
	}


	device->UnBindUnorderedAccessResources(0, ARRAYSIZE(uavs), threadID);
	device->UnBindResources(TEXSLOT_ONDEMAND0, ARRAYSIZE(resources), threadID);

	// Swap CURRENT alivelist with NEW alivelist
	SwapPtr(aliveList[0], aliveList[1]);
	emit -= (UINT)emit;

	if (DEBUG)
	{
		device->DownloadBuffer(counterBuffer, debugDataReadbackBuffer, &debugData, threadID);
	}
}

void wiEmittedParticle::Draw(GRAPHICSTHREAD threadID)
{
	GraphicsDevice* device = wiRenderer::GetDevice();
	device->EventBegin("EmittedParticle", threadID);

	if (wiRenderer::IsWireRender())
	{
		device->BindGraphicsPSO(&PSO_wire, threadID);
	}
	else
	{
		device->BindGraphicsPSO(&PSO[material->blendFlag][shaderType], threadID);
	}

	device->BindPrimitiveTopology(PRIMITIVETOPOLOGY::TRIANGLELIST, threadID);

	device->BindConstantBuffer(VS, constantBuffer, CB_GETBINDSLOT(EmittedParticleCB), threadID);

	GPUResource* res[] = {
		particleBuffer,
		aliveList[0]
	};
	device->TransitionBarrier(res, ARRAYSIZE(res), RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, threadID);
	device->BindResources(VS, res, 0, ARRAYSIZE(res), threadID);

	if (!wiRenderer::IsWireRender() && material->texture)
	{
		device->BindResource(PS, material->texture, TEXSLOT_ONDEMAND0, threadID);
	}

	device->DrawInstancedIndirect(indirectBuffers, ARGUMENTBUFFER_OFFSET_DRAWPARTICLES, threadID);

	device->EventEnd(threadID);
}


void wiEmittedParticle::CleanUp()
{
	SAFE_DELETE(particleBuffer);
	SAFE_DELETE(aliveList[0]);
	SAFE_DELETE(aliveList[1]);
	SAFE_DELETE(deadList);
	SAFE_DELETE(distanceBuffer);
	SAFE_DELETE(counterBuffer);
	SAFE_DELETE(indirectBuffers);
	SAFE_DELETE(constantBuffer);
	SAFE_DELETE(debugDataReadbackBuffer);
}



void wiEmittedParticle::LoadShaders()
{
	VertexShaderInfo* vsinfo = static_cast<VertexShaderInfo*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticleVS.cso", wiResourceManager::VERTEXSHADER));
	if (vsinfo != nullptr){
		vertexShader = vsinfo->vertexShader;
	}


	pixelShader[SOFT] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticlePS_soft.cso", wiResourceManager::PIXELSHADER));
	pixelShader[SOFT_DISTORTION] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticlePS_soft_distortion.cso", wiResourceManager::PIXELSHADER));
	pixelShader[SIMPLEST] = static_cast<PixelShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticlePS_simplest.cso", wiResourceManager::PIXELSHADER));
	
	
	kickoffUpdateCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_kickoffUpdateCS.cso", wiResourceManager::COMPUTESHADER));
	emitCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_emitCS.cso", wiResourceManager::COMPUTESHADER));
	simulateCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_simulateCS.cso", wiResourceManager::COMPUTESHADER));
	simulateCS_SORTING = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_simulateCS_SORTING.cso", wiResourceManager::COMPUTESHADER));
	simulateCS_DEPTHCOLLISIONS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_simulateCS_DEPTHCOLLISIONS.cso", wiResourceManager::COMPUTESHADER));
	simulateCS_SORTING_DEPTHCOLLISIONS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_simulateCS_SORTING_DEPTHCOLLISIONS.cso", wiResourceManager::COMPUTESHADER));

	kickoffSortCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_kickoffSortCS.cso", wiResourceManager::COMPUTESHADER));
	sortCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_sortCS.cso", wiResourceManager::COMPUTESHADER));
	sortInnerCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_sortInnerCS.cso", wiResourceManager::COMPUTESHADER));
	sortStepCS = static_cast<ComputeShader*>(wiResourceManager::GetShaderManager()->add(wiRenderer::SHADERPATH + "emittedparticle_sortStepCS.cso", wiResourceManager::COMPUTESHADER));


	GraphicsDevice* device = wiRenderer::GetDevice();

	for (int i = 0; i < BLENDMODE_COUNT; ++i)
	{
		GraphicsPSODesc desc;
		desc.vs = vertexShader;
		desc.bs = &blendStates[i];
		desc.rs = &rasterizerState;
		desc.dss = &depthStencilState;
		desc.numRTs = 1;
		desc.RTFormats[0] = wiRenderer::RTFormat_hdr;

		desc.ps = pixelShader[SOFT];
		device->CreateGraphicsPSO(&desc, &PSO[i][SOFT]);
		desc.ps = pixelShader[SOFT_DISTORTION];
		device->CreateGraphicsPSO(&desc, &PSO[i][SOFT_DISTORTION]);
		desc.ps = pixelShader[SIMPLEST];
		device->CreateGraphicsPSO(&desc, &PSO[i][SIMPLEST]);
	}

	{
		GraphicsPSODesc desc;
		desc.vs = vertexShader;
		desc.ps = pixelShader[SIMPLEST];
		desc.bs = &blendStates[BLENDMODE_ALPHA];
		desc.rs = &wireFrameRS;
		desc.dss = &depthStencilState;
		desc.numRTs = 1;
		desc.RTFormats[0] = wiRenderer::RTFormat_hdr;

		device->CreateGraphicsPSO(&desc, &PSO_wire);
	}

	{
		ComputePSODesc desc;

		desc.cs = kickoffUpdateCS;
		device->CreateComputePSO(&desc, &CPSO_kickoffUpdate);

		desc.cs = emitCS;
		device->CreateComputePSO(&desc, &CPSO_emit);

		desc.cs = simulateCS;
		device->CreateComputePSO(&desc, &CPSO_simulate);

		desc.cs = simulateCS_SORTING;
		device->CreateComputePSO(&desc, &CPSO_simulate_SORTING);

		desc.cs = simulateCS_DEPTHCOLLISIONS;
		device->CreateComputePSO(&desc, &CPSO_simulate_DEPTHCOLLISIONS);

		desc.cs = simulateCS_SORTING_DEPTHCOLLISIONS;
		device->CreateComputePSO(&desc, &CPSO_simulate_SORTING_DEPTHCOLLISIONS);

		desc.cs = kickoffSortCS;
		device->CreateComputePSO(&desc, &CPSO_kickoffSort);

		desc.cs = sortCS;
		device->CreateComputePSO(&desc, &CPSO_sort);

		desc.cs = sortInnerCS;
		device->CreateComputePSO(&desc, &CPSO_sortInner);

		desc.cs = sortStepCS;
		device->CreateComputePSO(&desc, &CPSO_sortStep);
	}

}
void wiEmittedParticle::LoadBuffers()
{
	GPUBufferDesc bd;

	bd.Usage = USAGE_DYNAMIC;
	bd.CPUAccessFlags = CPU_ACCESS_WRITE;
	bd.BindFlags = BIND_CONSTANT_BUFFER;
	bd.MiscFlags = 0;
	bd.ByteWidth = sizeof(SortConstants);
	sortCB = new GPUBuffer;
	wiRenderer::GetDevice()->CreateBuffer(&bd, nullptr, sortCB);


}
void wiEmittedParticle::SetUpStates()
{
	RasterizerStateDesc rs;
	rs.FillMode=FILL_SOLID;
	rs.CullMode=CULL_BACK;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=false;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	wiRenderer::GetDevice()->CreateRasterizerState(&rs,&rasterizerState);

	
	rs.FillMode=FILL_WIREFRAME;
	rs.CullMode=CULL_NONE;
	rs.FrontCounterClockwise=true;
	rs.DepthBias=0;
	rs.DepthBiasClamp=0;
	rs.SlopeScaledDepthBias=0;
	rs.DepthClipEnable=false;
	rs.MultisampleEnable=false;
	rs.AntialiasedLineEnable=false;
	wiRenderer::GetDevice()->CreateRasterizerState(&rs,&wireFrameRS);

	
	DepthStencilStateDesc dsd;
	dsd.DepthEnable = false;
	dsd.StencilEnable = false;
	wiRenderer::GetDevice()->CreateDepthStencilState(&dsd, &depthStencilState);

	
	BlendStateDesc bd;
	bd.RenderTarget[0].BlendEnable=true;
	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.IndependentBlendEnable=false;
	wiRenderer::GetDevice()->CreateBlendState(&bd,&blendStates[BLENDMODE_ALPHA]);

	bd.RenderTarget[0].BlendEnable=true;
	bd.RenderTarget[0].SrcBlend = BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = BLEND_ONE;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ZERO;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.IndependentBlendEnable=false;
	wiRenderer::GetDevice()->CreateBlendState(&bd, &blendStates[BLENDMODE_ADDITIVE]);

	bd.RenderTarget[0].BlendEnable = true;
	bd.RenderTarget[0].SrcBlend = BLEND_ONE;
	bd.RenderTarget[0].DestBlend = BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = COLOR_WRITE_ENABLE_ALL;
	bd.IndependentBlendEnable = false;
	wiRenderer::GetDevice()->CreateBlendState(&bd, &blendStates[BLENDMODE_PREMULTIPLIED]);

	bd.RenderTarget[0].BlendEnable = false;
	wiRenderer::GetDevice()->CreateBlendState(&bd, &blendStates[BLENDMODE_OPAQUE]);
}
void wiEmittedParticle::SetUpStatic()
{
	LoadBuffers();
	SetUpStates();
	LoadShaders();
}
void wiEmittedParticle::CleanUpStatic()
{
	SAFE_DELETE(vertexShader);
	for (int i = 0; i < ARRAYSIZE(pixelShader); ++i)
	{
		SAFE_DELETE(pixelShader[i]);
	}
	SAFE_DELETE(emitCS);
	SAFE_DELETE(simulateCS);
	SAFE_DELETE(simulateCS_SORTING);
	SAFE_DELETE(simulateCS_DEPTHCOLLISIONS);
	SAFE_DELETE(simulateCS_SORTING_DEPTHCOLLISIONS);
	SAFE_DELETE(sortCS);
	SAFE_DELETE(sortInnerCS);
	SAFE_DELETE(sortStepCS);
	SAFE_DELETE(sortCB);
}

void wiEmittedParticle::Serialize(wiArchive& archive)
{
	if (archive.IsReadMode())
	{
		archive >> emit;
		if (archive.GetVersion() < 9)
		{
			XMFLOAT4X4 transform4;
			XMFLOAT3X3 transform3;
			archive >> transform4;
			archive >> transform3;
		}
		archive >> name;
		archive >> materialName;
		archive >> size;
		archive >> random_factor;
		archive >> normal_factor;
		archive >> count;
		archive >> life;
		archive >> random_life;
		archive >> scaleX;
		archive >> scaleY;
		archive >> rotation;
		archive >> motionBlurAmount;
		if (archive.GetVersion() < 9)
		{
			string lightName;
			archive >> lightName;
		}
		if (archive.GetVersion() >= 11)
		{
			archive >> MAX_PARTICLES;
			archive >> SORTING;
		}
		if (archive.GetVersion() >= 12)
		{
			archive >> DEPTHCOLLISIONS;
		}
		if (archive.GetVersion() >= 14)
		{
			int tmp;
			archive >> tmp;
			shaderType = (PARTICLESHADERTYPE)tmp;
		}

	}
	else
	{
		archive << emit;
		archive << name;
		archive << materialName;
		archive << size;
		archive << random_factor;
		archive << normal_factor;
		archive << count;
		archive << life;
		archive << random_life;
		archive << scaleX;
		archive << scaleY;
		archive << rotation;
		archive << motionBlurAmount;
		if (archive.GetVersion() >= 11)
		{
			archive << MAX_PARTICLES;
			archive << SORTING;
		}
		if (archive.GetVersion() >= 12)
		{
			archive << DEPTHCOLLISIONS;
		}
		if (archive.GetVersion() >= 14)
		{
			archive << (int)shaderType;
		}
	}
}
