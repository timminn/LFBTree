/*
 * Utils.hpp
 *
 *  Created on: Apr 28, 2014
 *      Author: spatil
 */

#ifndef UTILS_HPP_
#define UTILS_HPP_

typedef unsigned __int128 uint128;

class Utils {
public:
	static const char DEFAULT_KEY;
	static const int MIN_KEYS;
	static const int MAX_KEYS;

	Utils();
	virtual ~Utils();

	static uint128 combine(long msb, long lsb);

	static inline uint128 InterlockedCompareExchange128(volatile uint128 * src,
			uint128 cmp, uint128 with) {
		__asm__ __volatile__
		(
				"lock cmpxchg16b %1"
				: "+A" ( cmp )
				, "+m" ( *src )
				: "b" ( (long long)with )
				, "c" ( (long long)(with>>64) )
				: "cc"
		);
		return cmp;
	}
};

#endif /* UTILS_HPP_ */
