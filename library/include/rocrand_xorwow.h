// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCRAND_XORWOW_H_
#define ROCRAND_XORWOW_H_

#ifndef FQUALIFIERS
#define FQUALIFIERS __forceinline__ __device__
#endif // FQUALIFIERS_

#include "rocrand_common.h"
#include "rocrand_xorwow_precomputed.h"

// G. Marsaglia, Xorshift RNGs, 2003
// http://www.jstatsoft.org/v08/i14/paper

#define ROCRAND_XORWOW_DEFAULT_SEED 0ULL

namespace rocrand_device {
namespace detail {

FQUALIFIERS
void copy_mat(unsigned int * dst, const unsigned int * src)
{
    for (int i = 0; i < XORWOW_SIZE; i++)
    {
        dst[i] = src[i];
    }
}

FQUALIFIERS
void copy_vec(unsigned int * dst, const unsigned int * src)
{
    for (int i = 0; i < XORWOW_N; i++)
    {
        dst[i] = src[i];
    }
}

FQUALIFIERS
void mul_mat_vec_inplace(const unsigned int * m, unsigned int * v)
{
    unsigned int r[XORWOW_N] = { 0 };
    // WORKAROUND: on HCC kernel fails with
    //   Memory access fault by GPU node-2 on address 0x1a14257000.
    //   Reason: Page not present or supervisor privilege.
    // Perhaps, the kernel's size is too big and disabled unrolling reduces it.
    #pragma unroll 1
    for (int i = 0; i < XORWOW_N; i++)
    {
        for (int j = 0; j < XORWOW_M; j++)
        {
            if (v[i] & (1 << j))
            {
                for (int k = 0; k < XORWOW_N; k++)
                {
                    r[k] ^= m[i * XORWOW_M * XORWOW_N + j * XORWOW_N + k];
                }
            }
        }
    }
    copy_vec(v, r);
}

FQUALIFIERS
void mul_mat_mat_inplace(unsigned int * a, const unsigned int * b)
{
    for (int i = 0; i < XORWOW_N * XORWOW_M; i++)
    {
        mul_mat_vec_inplace(b, a + i * XORWOW_N);
    }
}

} // end detail namespace

class xorwow_engine
{
public:
    struct xorwow_state
    {
        // Xorshift values (160 bits)
        unsigned int x[5];

        // Weyl sequence value
        unsigned int d;

        #ifndef ROCRAND_DETAIL_XORWOW_BM_NOT_IN_STATE
        // The Box–Muller transform requires two inputs to convert uniformly
        // distributed real values [0; 1] to normally distributed real values
        // (with mean = 0, and stddev = 1). Often user wants only one
        // normally distributed number, to save performance and random
        // numbers the 2nd value is saved for future requests.
        unsigned int boxmuller_float_state; // is there a float in boxmuller_float
        unsigned int boxmuller_double_state; // is there a double in boxmuller_double
        float boxmuller_float; // normally distributed float
        double boxmuller_double; // normally distributed double
        #endif

        FQUALIFIERS
        ~xorwow_state() { }
    };

    FQUALIFIERS
    xorwow_engine() : xorwow_engine(ROCRAND_XORWOW_DEFAULT_SEED, 0, 0) { }

    /// Initializes the internal state of the PRNG using
    /// seed value \p seed, goes to \p subsequence -th subsequence,
    /// and skips \p offset random numbers.
    ///
    /// A subsequence is 2^67 numbers long.
    FQUALIFIERS
    xorwow_engine(const unsigned long long seed,
                  const unsigned long long subsequence,
                  const unsigned long long offset)
    {
        m_state.x[0] = 123456789UL;
        m_state.x[1] = 362436069UL;
        m_state.x[2] = 521288629UL;
        m_state.x[3] = 88675123UL;
        m_state.x[4] = 5783321UL;

        m_state.d = 6615241UL;

        // TODO all constants below are from cuRAND
        const unsigned int s0 = static_cast<unsigned int>(seed) ^ 0xaad26b49UL;
        const unsigned int s1 = static_cast<unsigned int>(seed >> 32) ^ 0xf7dcefddUL;
        const unsigned int t0 = 1099087573UL * s0;
        const unsigned int t1 = 2591861531UL * s1;
        m_state.x[0] += t0;
        m_state.x[1] ^= t0;
        m_state.x[2] += t1;
        m_state.x[3] ^= t1;
        m_state.x[4] += t0;
        m_state.d += t1 + t0;

        discard_subsequence(subsequence);
        discard(offset);

        #ifndef ROCRAND_DETAIL_XORWOW_BM_NOT_IN_STATE
        m_state.boxmuller_float_state = 0;
        m_state.boxmuller_double_state = 0;
        #endif
    }

    FQUALIFIERS
    ~xorwow_engine() { }

    /// Advances the internal state to skip \p offset numbers.
    FQUALIFIERS
    void discard(unsigned long long offset)
    {
        #ifdef __HIP_DEVICE_COMPILE__
        jump(offset, d_xorwow_jump_matrices);
        #else
        jump(offset, h_xorwow_jump_matrices);
        #endif

        // Apply n steps to Weyl sequence value as well
        m_state.d += static_cast<unsigned int>(offset) * 362437;
    }

    /// Advances the internal state to skip \p subsequence subsequences.
    /// A subsequence is 2^67 numbers long.
    FQUALIFIERS
    void discard_subsequence(unsigned long long subsequence)
    {
        // Discard n * 2^67 samples
        #ifdef __HIP_DEVICE_COMPILE__
        jump(subsequence, d_xorwow_sequence_jump_matrices);
        #else
        jump(subsequence, h_xorwow_sequence_jump_matrices);
        #endif

        // d has the same value because 2^67 is divisible by 2^32 (d is 32-bit)
    }

    FQUALIFIERS
    unsigned int operator()()
    {
        return next();
    }

    FQUALIFIERS
    unsigned int next()
    {
        const unsigned int t = m_state.x[0] ^ (m_state.x[0] >> 2);
        m_state.x[0] = m_state.x[1];
        m_state.x[1] = m_state.x[2];
        m_state.x[2] = m_state.x[3];
        m_state.x[3] = m_state.x[4];
        m_state.x[4] = (m_state.x[4] ^ (m_state.x[4] << 4)) ^ (t ^ (t << 1));

        m_state.d += 362437;

        return m_state.d + m_state.x[4];
    }

protected:

    FQUALIFIERS
    void jump(unsigned long long v,
              const unsigned int jump_matrices[XORWOW_JUMP_MATRICES][XORWOW_SIZE])
    {
        // x~(n + v) = (A^v mod m)x~n mod m
        // The matrix (A^v mod m) can be precomputed for selected values of v.
        //
        // For XORWOW_JUMP_LOG2 = 2
        // xorwow_jump_matrices contains precomputed matrices:
        //   A^1, A^4, A^16...
        //
        // For XORWOW_JUMP_LOG2 = 2 and XORWOW_SEQUENCE_JUMP_LOG2 = 67
        // xorwow_sequence_jump_matrices contains precomputed matrices:
        //   A^(1 * 2^67), A^(4 * 2^67), A^(16 * 2^67)...
        //
        // Intermediate powers can calculated as multiplication of the powers above.
        // Powers after the last precomputed matrix can be calculated using
        // Exponentiation by squaring method.

        int mi = 0;
        while (v > 0 && mi < XORWOW_JUMP_MATRICES)
        {
            const int l = ((mi < XORWOW_JUMP_MATRICES - 1) ? XORWOW_JUMP_LOG2 : 1);
            for (int i = 0; i < (v & ((1 << l) - 1)); i++)
            {
                detail::mul_mat_vec_inplace(jump_matrices[mi], m_state.x);
            }
            mi++;
            v >>= l;
        }

        if (v > 0)
        {
            // All precomputed matrices are used, we need to use the last one
            // to create matrices of next powers of 2

            // WORKAROUND: this workaround is needed on HCC when unrolling
            // is not disabled in mul_mat_vec_inplace (more details there).
            #if defined(__HIP_PLATFORM_HCC__) && defined(__HIP_DEVICE_COMPILE__)
            // Linear version is used here instead of matrix squaring.
            // This means that large offsets will need a lot of time to process.

            for (v = v << 1; v > 0; v--)
            {
                detail::mul_mat_vec_inplace(jump_matrices[XORWOW_JUMP_MATRICES - 1], m_state.x);
            }
            #else // NVCC and host code

            unsigned int a[XORWOW_SIZE];
            unsigned int b[XORWOW_SIZE];

            detail::copy_mat(a, jump_matrices[XORWOW_JUMP_MATRICES - 1]);
            detail::copy_mat(b, a);

            // Exponentiation by squaring
            do
            {
                // Square the matrix
                detail::mul_mat_mat_inplace(a, b);
                detail::copy_mat(b, a);

                if (v & 1)
                {
                    detail::mul_mat_vec_inplace(b, m_state.x);
                }

                v >>= 1;
            } while (v > 0);

            #endif
        }
    }

protected:
    // State
    xorwow_state m_state;

    #ifndef ROCRAND_DETAIL_XORWOW_BM_NOT_IN_STATE
    friend class detail::engine_boxmuller_helper<xorwow_engine>;
    #endif

}; // xorwow_engine class

} // end namespace rocrand_device

typedef rocrand_device::xorwow_engine rocrand_state_xorwow;

FQUALIFIERS
void rocrand_init(const unsigned long long seed,
                  const unsigned long long subsequence,
                  const unsigned long long offset,
                  rocrand_state_xorwow * state)
{
    *state = rocrand_state_xorwow(seed, subsequence, offset);
}

FQUALIFIERS
unsigned int rocrand(rocrand_state_xorwow * state)
{
    return state->next();
}

FQUALIFIERS
void skipahead(unsigned long long offset, rocrand_state_xorwow * state)
{
    return state->discard(offset);
}

FQUALIFIERS
void skipahead_subsequence(unsigned long long subsequence, rocrand_state_xorwow * state)
{
    return state->discard_subsequence(subsequence);
}

#endif // ROCRAND_XORWOW_H_