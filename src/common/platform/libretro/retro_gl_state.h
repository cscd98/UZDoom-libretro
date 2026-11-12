#pragma once

#include "gl_load/gl_load.h"
#include <vector>

namespace Libretro
{
// The renderer caches GL state. Preserve it while the frontend uses the same
// context, including the current shader which its shader manager caches.
class RetroGLState
{
    struct TextureUnit { GLint texture, sampler; };
    std::vector<TextureUnit> units;

    static constexpr GLenum capabilities[] = {
        GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_MULTISAMPLE, GL_FRAMEBUFFER_SRGB, GL_DEPTH_CLAMP, GL_POLYGON_OFFSET_FILL,
        GL_DITHER, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE,
        GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1, GL_CLIP_DISTANCE2,
        GL_CLIP_DISTANCE3, GL_CLIP_DISTANCE4, GL_CLIP_DISTANCE5,
        GL_CLIP_DISTANCE6, GL_CLIP_DISTANCE7
    };

    GLboolean enabled[sizeof(capabilities) / sizeof(capabilities[0])];
    GLint program, vao, arrayBuffer, activeTexture, drawFramebuffer, readFramebuffer;
    GLint viewport[4], scissor[4], polygonMode[2], cullFace, frontFace, depthFunc;
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha, blendEqRGB, blendEqAlpha;
    GLfloat blendColor[4], polygonFactor, polygonUnits;
    GLdouble depthRange[2];
    GLboolean colorMask[4], depthMask;

    struct Stencil
    {
        GLint func, ref, valueMask, writeMask, fail, depthFail, depthPass;
    } frontStencil, backStencil;

    static void RestoreStencil(GLenum face, const Stencil &state)
    {
        glStencilFuncSeparate(face, state.func, state.ref, state.valueMask);
        glStencilMaskSeparate(face, state.writeMask);
        glStencilOpSeparate(face, state.fail, state.depthFail, state.depthPass);
    }

public:
    void Save(GLuint frontendFramebuffer = 0)
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);

        // A frontend resize may replace its FBO while the engine is yielded.
        // Zero is translated to the current frontend target by the GL loader.
        if (frontendFramebuffer && GLuint(drawFramebuffer) == frontendFramebuffer)
            drawFramebuffer = 0;

        if (frontendFramebuffer && GLuint(readFramebuffer) == frontendFramebuffer)
            readFramebuffer = 0;

        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_POLYGON_MODE, polygonMode);
        glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);
        glGetIntegerv(GL_FRONT_FACE, &frontFace);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetDoublev(GL_DEPTH_RANGE, depthRange);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
        glGetFloatv(GL_BLEND_COLOR, blendColor);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polygonFactor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polygonUnits);
        glGetIntegerv(GL_STENCIL_FUNC, &frontStencil.func);
        glGetIntegerv(GL_STENCIL_REF, &frontStencil.ref);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &frontStencil.valueMask);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &frontStencil.writeMask);
        glGetIntegerv(GL_STENCIL_FAIL, &frontStencil.fail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &frontStencil.depthFail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &frontStencil.depthPass);
        glGetIntegerv(GL_STENCIL_BACK_FUNC, &backStencil.func);
        glGetIntegerv(GL_STENCIL_BACK_REF, &backStencil.ref);
        glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK, &backStencil.valueMask);
        glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &backStencil.writeMask);
        glGetIntegerv(GL_STENCIL_BACK_FAIL, &backStencil.fail);
        glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL, &backStencil.depthFail);
        glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &backStencil.depthPass);

        for (unsigned i = 0; i < sizeof(enabled) / sizeof(enabled[0]); ++i)
            enabled[i] = glIsEnabled(capabilities[i]);

        GLint count = 0;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &count);
        units.resize(count);

        for (unsigned i = 0; i < units.size(); ++i)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &units[i].texture);
            glGetIntegerv(GL_SAMPLER_BINDING, &units[i].sampler);
        }

        glActiveTexture(activeTexture);
    }

    void Restore()
    {
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glPolygonMode(GL_FRONT_AND_BACK, polygonMode[0]);
        glCullFace(cullFace);
        glFrontFace(frontFace);
        glDepthFunc(depthFunc);
        glDepthRange(depthRange[0], depthRange[1]);
        glDepthMask(depthMask);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
        glBlendEquationSeparate(blendEqRGB, blendEqAlpha);
        glBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
        glPolygonOffset(polygonFactor, polygonUnits);
        RestoreStencil(GL_FRONT, frontStencil);
        RestoreStencil(GL_BACK, backStencil);

        for (unsigned i = 0; i < sizeof(enabled) / sizeof(enabled[0]); ++i)
        {
            if (enabled[i]) glEnable(capabilities[i]);
            else glDisable(capabilities[i]);
        }

        for (unsigned i = 0; i < units.size(); ++i)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, units[i].texture);
            glBindSampler(i, units[i].sampler);
        }

        glActiveTexture(activeTexture);
    }
};
}
