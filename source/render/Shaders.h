#pragma once

#include <string>

/**
    Every shader the renderer uses.

    ---------------------------------------------------------------------------
    The rule that matters here
    ---------------------------------------------------------------------------

    **The paper buffer does not hold a picture. It holds optical density.**

    Ink is subtractive: a pen removes light from a sheet rather than adding it,
    and two coats are darker than one because the light passes through twice as
    much pigment. So the buffer accumulates `k * amount`, where `k` is the pen's
    absorption coefficient per channel (`machine/Pens.h`), and the display pass
    is the only place that turns it into light:

        shown = paper * exp( -density )

    Everything worth having falls out of that and none of it is applied on top.
    One unit of a pen whose colour is C reads back as exactly C. Two units read
    back as C squared. Red crossing blue reads back as the product of the two,
    which is the muddy near-black it is on the table and which additive blending
    cannot produce at all. And because the buffer never contained a colour, the
    operator can change the paper, the palette, or the pen mid-drawing without
    anything already on the sheet being wrong.

    **Nothing computes `1 / v`.** The ink pass deposits a fixed quantum per step
    interval and spreads it over the paper distance the nib covered in that
    interval, so a pen that slows down lays a darker line as a matter of
    arithmetic rather than as an effect. The closed form is the convolution of a
    uniform segment of length L carrying amount A with an isotropic Gaussian nib
    of width sigma:

        f(u,v) = (A/L) * G_sigma(v) * [ Phi(u/sigma) - Phi((u-L)/sigma) ]

    It conserves ink exactly for any L and any sigma -- which is what makes the
    total deposit independent of how many steps the crank chose to emit, tested
    by `cgtest --detail` -- and as L goes to zero the L in the denominator
    cancels against the vanishing CDF difference, leaving a finite blot. That is
    why a pen sitting still needs no clamp and no divide-by-zero guard. Borrowed
    wholesale from `vectrix`, where the same integral is a beam rather than a
    nib.

    ---------------------------------------------------------------------------
    GLSL reserved words
    ---------------------------------------------------------------------------

    `sample`, `input`, `output`, `filter`, `common`, `active`, `shared`, `half`
    and `flat` are reserved. This plugin is about pen *samples* through a paper
    *filter*, so the natural names are nearly all taken: hence `stepA` / `stepB`,
    `halfLen`, `ClipTexture`. Shader errors surface only at runtime, in the
    diagnostics log, as "the plugin does nothing".

    ---------------------------------------------------------------------------
    Assembly
    ---------------------------------------------------------------------------

    `#version 410 core` throughout: macOS caps at GL 4.1, so there is no compute
    stage and no image load/store, and every accumulation is done by the
    blender. Two preludes, because `fwidth` is fragment-only and a vertex stage
    compiles the whole body whether it calls it or not.
*/
namespace cogwheel::shaders
{

/// `#version` + shared constants. For vertex stages.
std::string vertexSource( const char* body );

/// `#version` + shared constants + shared helpers. For fragment stages.
std::string fragmentSource( const char* body );

/// Full-screen quad, matching `ffglex::FFGLScreenQuad`'s attribute layout.
const std::string& screenVertex();

/// The nib: one instanced quad per step interval, additive, into RGBA32F.
const std::string& inkVertex();
const std::string& inkFragment();

/// The sheet lightening. Drawn with `glBlendFunc( GL_ZERO, GL_SRC_COLOR )`, so
/// it multiplies the buffer in place -- which is why the paper needs no
/// ping-pong and no second allocation.
const std::string& fadeFragment();

/// The sheet: paper, grain, Beer's law, the gear overlay, and the composite
/// onto the host's framebuffer.
const std::string& sheetFragment();

} // namespace cogwheel::shaders
