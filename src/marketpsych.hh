/* MarketPsych defined rounding math.
 */

#ifndef __MARKETPSYCH_HH__
#define __MARKETPSYCH_HH__
#pragma once

#include <cmath>
#include <cstdint>

/* RFA 7.2 */
#include <rfa/rfa.hh>

namespace marketpsych
{

static inline
double
round_half_up (double x)
{
	return std::floor (x + 0.5);
}

static inline
int64_t
mantissa (double x)
{
	return (int64_t) round_half_up (x * 1000000.0);
}

static inline
double
round (double x)
{
	return (double) mantissa (x) / 1000000.0;
}

} // namespace marketpsych

#endif /* __MARKETPSYCH_HH__ */

/* eof */