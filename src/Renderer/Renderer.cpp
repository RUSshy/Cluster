#include "Renderer.h"

#include "Scene/Scene.h"
#include <bigg.hpp>
#include <bx/macros.h>
#include <bx/string.h>
#include <bx/math.h>
#include <glm/common.hpp>
#include <glm/gtx/component_wise.hpp>
#include <glm/gtc/color_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_operation.hpp>

bgfx::VertexDecl Renderer::PosTexCoord0Vertex::decl;

Renderer::Renderer(const Scene* scene) :
    tonemappingMode(TonemappingMode::NONE),
    scene(scene),
    width(0),
    height(0),
    clearColor(0),
    time(0.0f),
    frameBuffer(BGFX_INVALID_HANDLE),
    blitTriangleBuffer(BGFX_INVALID_HANDLE),
    blitProgram(BGFX_INVALID_HANDLE),
    blitSampler(BGFX_INVALID_HANDLE),
    camPosUniform(BGFX_INVALID_HANDLE),
    normalMatrixUniform(BGFX_INVALID_HANDLE),
    exposureVecUniform(BGFX_INVALID_HANDLE),
    tonemappingModeVecUniform(BGFX_INVALID_HANDLE)
{
}

Renderer::~Renderer()
{
}

void Renderer::initialize()
{
    PosTexCoord0Vertex::init();

    blitSampler = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
    camPosUniform = bgfx::createUniform("u_camPos", bgfx::UniformType::Vec4);
    normalMatrixUniform = bgfx::createUniform("u_normalMatrix", bgfx::UniformType::Mat3);
    exposureVecUniform = bgfx::createUniform("u_exposureVec", bgfx::UniformType::Vec4);
    tonemappingModeVecUniform = bgfx::createUniform("u_tonemappingModeVec", bgfx::UniformType::Vec4);

    float bottomUV = bgfx::getCaps()->originBottomLeft ? 0.0f :  1.0f;
    float topUV    = bgfx::getCaps()->originBottomLeft ? 2.0f : -1.0f;
    constexpr float BOTTOM = -1.0f, TOP = 3.0f, LEFT = -1.0f, RIGHT = 3.0f;
    PosTexCoord0Vertex vertices[3] = {
        { LEFT,  BOTTOM, 0.0f, 0.0f, bottomUV },
        { LEFT,  TOP,    0.0f, 0.0f, topUV },
        { RIGHT, BOTTOM, 0.0f, 2.0f, bottomUV }
    };

    blitTriangleBuffer = bgfx::createVertexBuffer(bgfx::copy(&vertices, sizeof(vertices)), PosTexCoord0Vertex::decl);

    char vsName[128], fsName[128];
    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_tonemap.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_tonemap.bin");
    blitProgram = bigg::loadProgram(vsName, fsName);

    pbr.initialize();
    lights.initialize();

    onInitialize();
}

void Renderer::reset(uint16_t width, uint16_t height)
{
    if(!bgfx::isValid(frameBuffer))
    {
        frameBuffer = createFrameBuffer(true, true);
        bgfx::setName(frameBuffer, "Render framebuffer (pre-postprocessing)");
    }
    this->width = width;
    this->height = height;

    onReset();
}

void Renderer::render(float dt)
{
    time += dt;

    if(scene->loaded)
    {
        glm::vec4 camPos = glm::vec4(scene->camera.position(), 1.0f);
        bgfx::setUniform(camPosUniform, glm::value_ptr(camPos));

        // tonemapping expects linear colors
        glm::vec3 linear = glm::convertSRGBToLinear(scene->skyColor);
        glm::u8vec3 result = glm::u8vec3(glm::round(glm::clamp(linear, 0.0f, 1.0f) * 255.0f));
        clearColor = (result[0] << 24) | (result[1] << 16) | (result[2] << 8) | 255;
    }
    else
    {
        clearColor = 0x303030FF;
    }

    // bigg doesn't do this
    bgfx::setViewName(MAX_VIEW + 1, "imgui");

    onRender(dt);
    blitToScreen(MAX_VIEW);
}

void Renderer::shutdown()
{
    onShutdown();

    pbr.shutdown();
    lights.shutdown();

    bgfx::destroy(blitProgram);
    bgfx::destroy(blitSampler);
    bgfx::destroy(camPosUniform);
    bgfx::destroy(normalMatrixUniform);
    bgfx::destroy(exposureVecUniform);
    bgfx::destroy(tonemappingModeVecUniform);
    bgfx::destroy(blitTriangleBuffer);
    if(bgfx::isValid(frameBuffer))
        bgfx::destroy(frameBuffer);

    blitProgram = BGFX_INVALID_HANDLE;
    blitSampler = camPosUniform = normalMatrixUniform = exposureVecUniform = tonemappingModeVecUniform = BGFX_INVALID_HANDLE;
    blitTriangleBuffer = BGFX_INVALID_HANDLE;
    frameBuffer = BGFX_INVALID_HANDLE;
}

void Renderer::setVariable(const std::string& name, const std::string& val)
{
    variables[name] = val;
}

void Renderer::setTonemappingMode(TonemappingMode mode)
{
    tonemappingMode = mode;
}

bool Renderer::supported()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    return (caps->formats[bgfx::TextureFormat::RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA) != 0;
}

void Renderer::setViewProjection(bgfx::ViewId view)
{
    // view matrix
    viewMat = scene->camera.matrix();
    // projection matrix
    bx::mtxProj(glm::value_ptr(projMat),
                scene->camera.fov,
                float(width) / height,
                scene->camera.zNear,
                scene->camera.zFar,
                bgfx::getCaps()->homogeneousDepth,
                bx::Handness::Left);
    bgfx::setViewTransform(view, glm::value_ptr(viewMat), glm::value_ptr(projMat));
}

void Renderer::setNormalMatrix(const glm::mat4& modelMat)
{
    // usually the normal matrix is based on the model view matrix
    // but shading is done in world space (not eye space) so it's just the model matrix
    //glm::mat4 modelViewMat = viewMat * modelMat;

    // if we don't do non-uniform scaling, the normal matrix is the same as the model-view matrix
    // (only the magnitude of the normal is changed, but we normalize either way)
    //glm::mat3 normalMat = glm::mat3(modelMat);

    // use adjugate instead of inverse
    // see https://github.com/graphitemaster/normals_revisited#the-details-of-transforming-normals
    // cofactor is the transpose of the adjugate
    glm::mat3 normalMat = glm::transpose(glm::adjugate(glm::mat3(modelMat)));
    bgfx::setUniform(normalMatrixUniform, glm::value_ptr(normalMat));
}

void Renderer::blitToScreen(bgfx::ViewId view)
{
    bgfx::setViewName(view, "Tonemapping");
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    bgfx::setViewRect(view, 0, 0, width, height);
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    bgfx::setState(BGFX_STATE_WRITE_RGB);
    bgfx::TextureHandle frameBufferTexture = bgfx::getTexture(frameBuffer);
    bgfx::setTexture(0, blitSampler, frameBufferTexture);
    float exposureVec[4] = { scene->loaded ? scene->camera.exposure : 1.0f };
    bgfx::setUniform(exposureVecUniform, exposureVec);
    float tonemappingModeVec[4] = { (float)tonemappingMode };
    bgfx::setUniform(tonemappingModeVecUniform, tonemappingModeVec);
    bgfx::setVertexBuffer(0, blitTriangleBuffer);
    bgfx::submit(view, blitProgram);
}

bgfx::FrameBufferHandle Renderer::createFrameBuffer(bool hdr, bool depth)
{
    bgfx::TextureHandle textures[2];
    uint8_t attachments = 0;

    // BGFX_TEXTURE_READ_BACK is not supported for render targets?
    // TODO try blitting for screenshots (new texture with BGFX_TEXTURE_BLIT_DST and BGFX_TEXTURE_READ_BACK)

    uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx::TextureFormat::Enum format = hdr
                                       ? bgfx::TextureFormat::RGBA16F
                                       : bgfx::TextureFormat::BGRA8;
    if(bgfx::isTextureValid(0, false, 1, format, flags))
    {
        textures[attachments++] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, format, flags);
    }
    // TODO error out

    if(depth)
    {
        flags = BGFX_TEXTURE_RT_WRITE_ONLY;
        bgfx::TextureFormat::Enum depthFormat = bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D16, flags)
                                                ? bgfx::TextureFormat::D16
                                                : bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8, flags)
                                                  ? bgfx::TextureFormat::D24S8
                                                  : bgfx::TextureFormat::D32;
        textures[attachments++] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, depthFormat, flags);
    }

    bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(attachments, textures, true);

    if(!bgfx::isValid(fb))
        Log->warn("Failed to create framebuffer");

    return fb;
}

const char* Renderer::shaderDir()
{
    const char* path = "???";

    switch(bgfx::getRendererType())
    {
        case bgfx::RendererType::Noop:
        case bgfx::RendererType::Direct3D9:
            path = "shaders/dx9/";
            break;
        case bgfx::RendererType::Direct3D11:
        case bgfx::RendererType::Direct3D12:
            path = "shaders/dx11/";
            break;
        case bgfx::RendererType::Gnm:
            break;
        case bgfx::RendererType::Metal:
            path = "shaders/metal/";
            break;
        case bgfx::RendererType::OpenGL:
            path = "shaders/glsl/";
            break;
        case bgfx::RendererType::OpenGLES:
            path = "shaders/essl/";
            break;
        case bgfx::RendererType::Vulkan:
            path = "shaders/spirv/";
            break;
        default:
            assert(false);
            break;
    }

    return path;
}
