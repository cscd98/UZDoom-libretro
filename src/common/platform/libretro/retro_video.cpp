#include "retro_platform.h"
#ifdef HAVE_VULKAN
#include "vulkan/system/vk_renderdevice.h"
#endif
#include "gl_sysfb.h"
#include "gl_framebuffer.h"
#ifdef HAVE_GLES2
#include "gles_framebuffer.h"
#endif
#include "hardware.h"
#include "i_system.h"
#include "c_cvars.h"
#include "printf.h"
#ifdef _WIN32
#include "retro_gl_state.h"
#endif

IVideo *Video = nullptr;
CVAR(Bool, gl_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, gl_es, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

class RetroVideo : public IVideo
{
public:
    void DumpAdapters() override
    {
        Printf("Display managed by the libretro frontend\n");
    }

    DFrameBuffer *CreateFrameBuffer() override
    {
#ifdef HAVE_VULKAN
        if (Libretro::UseVulkan)
            return new VulkanRenderDevice(nullptr, false, nullptr);
#endif
#ifdef HAVE_GLES2
        if (Libretro::GLES)
            return new OpenGLESRenderer::OpenGLFrameBuffer(nullptr, false);
#endif
        return new OpenGLRenderer::OpenGLFrameBuffer(nullptr, false);
    }
};

void I_InitGraphics() { Video = new RetroVideo; }

void I_ShutdownGraphics()
{
    auto old = screen;
    screen = nullptr;
    delete old;
    delete Video;
    Video = nullptr;
}

SystemBaseFrameBuffer::SystemBaseFrameBuffer(void *, bool) : DFrameBuffer(Libretro::Width, Libretro::Height) {}
bool SystemBaseFrameBuffer::IsFullscreen()
{
    return false;
}

int SystemBaseFrameBuffer::GetClientWidth()
{
    return Libretro::Width;
}

int SystemBaseFrameBuffer::GetClientHeight()
{
    return Libretro::Height;
}

void SystemBaseFrameBuffer::ToggleFullscreen(bool)
{
}

void SystemBaseFrameBuffer::SetWindowSize(int, int)
{
} // Geometry is selected by core options.

SystemGLFrameBuffer::SystemGLFrameBuffer(void *monitor, bool fullscreen) : SystemBaseFrameBuffer(monitor, fullscreen) {}
SystemGLFrameBuffer::~SystemGLFrameBuffer() = default;

int SystemGLFrameBuffer::GetClientWidth()
{
    return Libretro::Width;
}

int SystemGLFrameBuffer::GetClientHeight()
{
    return Libretro::Height;
}

void SystemGLFrameBuffer::SetVSync(bool)
{
} // The frontend controls presentation pacing.

void SystemGLFrameBuffer::SwapBuffers()
{
    Libretro::Present();
}

// Resolve every GL symbol through the current frontend context. Translate the
// standalone default framebuffer to the frontend target, which may be nonzero.
namespace
{
using BindFramebuffer = void (*)(unsigned, unsigned);
BindFramebuffer bindFramebuffer = nullptr;

void BindFrontendFramebuffer(unsigned target, unsigned framebuffer)
{
    if (!framebuffer && Libretro::Hardware.get_current_framebuffer)
        framebuffer = static_cast<unsigned>(Libretro::Hardware.get_current_framebuffer());

    bindFramebuffer(target, framebuffer);
}
}

extern "C" void *LibretroGLGetProcAddress(const char *name)
{
    if (!Libretro::Hardware.get_proc_address)
        return nullptr;

    auto address = Libretro::Hardware.get_proc_address(name);

    if (!strcmp(name, "glBindFramebuffer") || !strcmp(name, "glBindFramebufferEXT"))
    {
        bindFramebuffer = reinterpret_cast<BindFramebuffer>(address);
        return address ? reinterpret_cast<void *>(&BindFrontendFramebuffer) : nullptr;
    }

    return reinterpret_cast<void *>(address);
}

namespace Libretro
{
#ifdef _WIN32
namespace { RetroGLState engineGLState; }
#endif

void SaveGLState()
{
#ifdef _WIN32
    if (!UseVulkan && !GLES)
        engineGLState.Save(Hardware.get_current_framebuffer ? GLuint(Hardware.get_current_framebuffer()) : 0);
#endif
}

void RestoreGLState()
{
#ifdef _WIN32
    if (!UseVulkan && !GLES)
        engineGLState.Restore();
#endif
}

void ResizeOutput()
{
    if (screen) screen->DFrameBuffer::Update();
}
} // namespace Libretro
