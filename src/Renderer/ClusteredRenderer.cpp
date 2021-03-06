#include "ClusteredRenderer.h"

#include "Scene/Scene.h"
#include <bigg.hpp>
#include <bx/string.h>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>

ClusteredRenderer::ClusteredRenderer(const Scene* scene) :
    Renderer(scene),
    clusterBuildingComputeProgram(BGFX_INVALID_HANDLE),
    lightCullingComputeProgram(BGFX_INVALID_HANDLE),
    lightingProgram(BGFX_INVALID_HANDLE),
    debugVisProgram(BGFX_INVALID_HANDLE)
{
}

bool ClusteredRenderer::supported()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    return Renderer::supported() &&
           // compute shader
           (caps->supported & BGFX_CAPS_COMPUTE) != 0 &&
           // 32-bit index buffers, used for light grid structure
           // D3D12 doesn't report this but it works fine...
           // TODO try again with updated bgfx
           ((caps->supported & BGFX_CAPS_INDEX32) != 0 || caps->rendererType == bgfx::RendererType::Direct3D12) &&
           // fragment depth available in fragment shader
           (caps->supported & BGFX_CAPS_FRAGMENT_DEPTH) != 0;
}

void ClusteredRenderer::onInitialize()
{
    // OpenGL backend: uniforms must be created before loading shaders
    clusters.initialize();

    char csName[128], vsName[128], fsName[128];

    bx::snprintf(csName, BX_COUNTOF(csName), "%s%s", shaderDir(), "cs_clustered_clusterbuilding.bin");
    clusterBuildingComputeProgram = bgfx::createProgram(bigg::loadShader(csName), true);

    bx::snprintf(csName, BX_COUNTOF(csName), "%s%s", shaderDir(), "cs_clustered_lightculling.bin");
    lightCullingComputeProgram = bgfx::createProgram(bigg::loadShader(csName), true);

    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_clustered.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_clustered.bin");
    lightingProgram = bigg::loadProgram(vsName, fsName);

    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_clustered_debug_vis.bin");
    debugVisProgram = bigg::loadProgram(vsName, fsName);
}

void ClusteredRenderer::onRender(float dt)
{
    enum : bgfx::ViewId
    {
        vClusterBuilding = 0,
        vLightCulling,
        vLighting
    };

    bgfx::setViewClear(vLighting, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearColor, 1.0f, 0);
    bgfx::setViewRect(vLighting, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vLighting, frameBuffer);
    bgfx::touch(vLighting);

    if(!scene->loaded)
        return;

    // D3D12 crashes if this is not set, even for compute only views
    // TODO try again with updated bgfx
    bgfx::setViewFrameBuffer(vClusterBuilding, BGFX_INVALID_HANDLE);
    bgfx::setViewFrameBuffer(vLightCulling, BGFX_INVALID_HANDLE);

    clusters.setUniforms(scene, width, height);

    // set u_viewRect for screen2Eye to work correctly
    bgfx::setViewRect(vClusterBuilding, 0, 0, width, height);
    bgfx::setViewRect(vLightCulling, 0, 0, width, height);

    // cluster building needs u_invProj to transform screen coordinates to eye space
    setViewProjection(vClusterBuilding);
    // light culling needs u_view to transform lights to eye space
    setViewProjection(vLightCulling);

    {
        bgfx::setViewName(vClusterBuilding, "Cluster building pass (compute)");

        clusters.bindBuffers(false); // write access, all buffers

        bgfx::dispatch(vClusterBuilding,
                       clusterBuildingComputeProgram,
                       ClusterShader::CLUSTERS_X,
                       ClusterShader::CLUSTERS_Y,
                       ClusterShader::CLUSTERS_Z);
    }

    {
        bgfx::setViewName(vLightCulling, "Clustered light culling pass (compute)");

        lights.bindLights(scene);
        clusters.bindBuffers(false); // write access, all buffers

        bgfx::dispatch(vLightCulling,
                       lightCullingComputeProgram,
                       1,
                       1,
                       ClusterShader::CLUSTERS_Z / ClusterShader::CLUSTERS_Z_THREADS);
    }

    bgfx::setViewName(vLighting, "Clustered lighting pass");

    setViewProjection(vLighting);

    uint64_t state = BGFX_STATE_DEFAULT & ~BGFX_STATE_CULL_MASK;

    bool debugVis = variables["DEBUG_VIS"] == "true";
    bgfx::ProgramHandle program = debugVis ? debugVisProgram : lightingProgram;

    for(const Mesh& mesh : scene->meshes)
    {
        glm::mat4 model = glm::identity<glm::mat4>();
        bgfx::setTransform(glm::value_ptr(model));
        setNormalMatrix(model);
        bgfx::setVertexBuffer(0, mesh.vertexBuffer);
        bgfx::setIndexBuffer(mesh.indexBuffer);
        const Material& mat = scene->materials[mesh.material];
        uint64_t materialState = pbr.bindMaterial(mat);
        lights.bindLights(scene);
        clusters.bindBuffers();
        bgfx::setState(state | materialState);
        bgfx::submit(vLighting, program);
    }
}

void ClusteredRenderer::onShutdown()
{
    clusters.shutdown();

    bgfx::destroy(clusterBuildingComputeProgram);
    bgfx::destroy(lightCullingComputeProgram);
    bgfx::destroy(lightingProgram);
    bgfx::destroy(debugVisProgram);

    clusterBuildingComputeProgram = lightCullingComputeProgram = lightingProgram = debugVisProgram =
        BGFX_INVALID_HANDLE;
}
