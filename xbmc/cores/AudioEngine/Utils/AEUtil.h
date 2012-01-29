#pragma once
/*
 *      Copyright (C) 2005-2010 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "../AEAudioFormat.h"
#include "utils/StdString.h"
#include "PlatformDefs.h"
#include <math.h>

#ifdef __SSE__
#include <xmmintrin.h>
#else
#define __m128 void
#endif

#ifdef __GNUC__
  #define MEMALIGN(b, x) x __attribute__((aligned(b)))
#else
  #define MEMALIGN(b, x) __declspec(align(b)) x
#endif

class CAEUtil
{
private:
  static unsigned int m_seed;
  #ifdef __SSE__
    static __m128i m_sseSeed;
  #endif

public:
  static CAEChannelInfo          GuessChLayout     (const unsigned int channels);
  static const char*             GetStdChLayoutName(const enum AEStdChLayout layout);
  static const unsigned int      DataFormatToBits  (const enum AEDataFormat dataFormat);
  static const char*             DataFormatToStr   (const enum AEDataFormat dataFormat);

  static inline float SoftClamp(float x)
  {
#if 1
    /*
       This is a rational function to approximate a tanh-like soft clipper.
       It is based on the pade-approximation of the tanh function with tweaked coefficients.
       See: http://www.musicdsp.org/showone.php?id=238
    */
         if (x < -3.0f) return -1.0f;
    else if (x >  3.0f) return  1.0f;
    float y = x * x;
    return x * (27.0f + y) / (27.0f + 9.0f * y);
#else
    /* slower method using tanh, but more accurate */

    static const double k = 0.9f;
    /* perform a soft clamp */
         if (x >  k) x = (float) (tanh((x - k) / (1 - k)) * (1 - k) + k);
    else if (x < -k) x = (float) (tanh((x + k) / (1 - k)) * (1 - k) - k);

    /* hard clamp anything still outside the bounds */
    if (x >  1.0f) return  1.0f;
    if (x < -1.0f) return -1.0f;

    /* return the final sample */
    return x;
#endif
  }

  #ifdef __SSE__
  static void SSEMulAddArray  (float *data, float *add, const float mul, uint32_t count);
  static void SSEMulClampArray(float *data, const float mul, uint32_t count);
  static void SSEMulArray     (float *data, const float mul, uint32_t count);
  #endif

  /*
    Rand implementations based on:
    http://software.intel.com/en-us/articles/fast-random-number-generator-on-the-intel-pentiumr-4-processor/
    This is NOT safe for crypto work, but perfectly fine for audio usage (dithering)
  */
  static inline float FloatRand1(const float min, const float max)
  {
    const float delta  = (max - min) / 2;
    const float factor = delta / (float)INT32_MAX;
    return ((float)(m_seed = (214013 * m_seed + 2531011)) * factor) - delta;
  }

  static inline void FloatRand4(const float min, const float max, float result[4], __m128 *sseresult = NULL)
  {
    #ifdef __SSE__
      /*
        this method may be called from other SSE code, we need
        to calculate the delta & factor using SSE as the FPU
        state is unknown and _mm_clear() is expensive.
      */
      MEMALIGN(16, static const __m128 point5  ) = _mm_set_ps1(0.5f);
      MEMALIGN(16, static const __m128 int32max) = _mm_set_ps1((const float)INT32_MAX);
      MEMALIGN(16, __m128 f) = _mm_div_ps(
        _mm_mul_ps(
          _mm_sub_ps(
            _mm_set_ps1(max),
            _mm_set_ps1(min)
          ),
          point5
        ),
        int32max
      );

      MEMALIGN(16, __m128i cur_seed_split);
      MEMALIGN(16, __m128i multiplier);
      MEMALIGN(16, __m128i adder);
      MEMALIGN(16, __m128i mod_mask);
      MEMALIGN(16, __m128 res);
      MEMALIGN(16, static const unsigned int mult  [4]) = {214013, 17405, 214013, 69069};
      MEMALIGN(16, static const unsigned int gadd  [4]) = {2531011, 10395331, 13737667, 1};
      MEMALIGN(16, static const unsigned int mask  [4]) = {0xFFFFFFFF, 0, 0xFFFFFFFF, 0};

      adder          = _mm_load_si128((__m128i*)gadd);
      multiplier     = _mm_load_si128((__m128i*)mult);
      mod_mask       = _mm_load_si128((__m128i*)mask);
      cur_seed_split = _mm_shuffle_epi32(m_sseSeed, _MM_SHUFFLE(2, 3, 0, 1));

      m_sseSeed      = _mm_mul_epu32(m_sseSeed, multiplier); 
      multiplier     = _mm_shuffle_epi32(multiplier, _MM_SHUFFLE(2, 3, 0, 1));
      cur_seed_split = _mm_mul_epu32(cur_seed_split, multiplier);
    
      m_sseSeed      = _mm_and_si128(m_sseSeed, mod_mask);
      cur_seed_split = _mm_and_si128(cur_seed_split, mod_mask);
      cur_seed_split = _mm_shuffle_epi32(cur_seed_split, _MM_SHUFFLE(2, 3, 0, 1));
      m_sseSeed      = _mm_or_si128(m_sseSeed, cur_seed_split);
      m_sseSeed      = _mm_add_epi32(m_sseSeed, adder);

      /* adjust the value to the range requested */
      res = _mm_cvtepi32_ps(m_sseSeed);
      if (sseresult)
        *sseresult = _mm_mul_ps(res, f);
      else
      {
        res = _mm_mul_ps(res, f);
        _mm_storeu_ps(result, res);

        /* returning a float array, so cleanup */
        _mm_empty();
      }

    #else
      const float delta  = (max - min) / 2.0f;
      const float factor = delta / (float)INT32_MAX;

      /* cant return sseresult if we are not using SSE intrinsics */
      ASSERT(result && !sseresult);

      result[0] = ((float)(m_seed = (214013 * m_seed + 2531011)) * factor) - delta;
      result[1] = ((float)(m_seed = (214013 * m_seed + 2531011)) * factor) - delta;
      result[2] = ((float)(m_seed = (214013 * m_seed + 2531011)) * factor) - delta;
      result[3] = ((float)(m_seed = (214013 * m_seed + 2531011)) * factor) - delta;
    #endif
  }
};
