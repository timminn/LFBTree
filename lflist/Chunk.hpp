/*
 * Chunk.hpp
 *
 *  Created on: Apr 28, 2014
 *      Author: spatil
 */

#ifndef CHUNK_HPP_
#define CHUNK_HPP_

#include <thread>
#include <atomic>
#include <cstdbool>
#include <cstdlib>
#include <iostream>
#include <limits.h>

#include "Utils.hpp"
#include "Entry.hpp"

#define SET_FREEZE_STATE_MASK 7
#define UNSET_FREEZE_STATE_MASK 0xfffffffffffffff8
#define SET_SWAPPED_BIT_MASK 1
#define UNSET_SWAPPED_BIT_MASK 0xfffffffffffffffe

//namespace lflist {

enum RecovType {
	SPLIT, MERGE, COPY
};

enum TriggerType {
	NONE, INSERT, DELETE, ENSLAVE
};

enum ReturnCode {
	SUCCESS_THIS, SUCCESS_OTHER, EXISTED
};

enum FreezeState {
	NO_FREEZE = 0, INTERNAL_FREEZE, EXTERNAL_FREEZE
};

template<class TData>
class Chunk {
private:

	int MIN, MAX;
	std::atomic<long> counter;		// number of entries currently allocated
	Entry *head;
	Entry *entriesArray;
	Chunk *newChunk;
	std::atomic<long> mergeBuddy;			// merge ptr and freeze state
	std::atomic<Chunk *> nextChunk;

public:

	static __thread Entry **prev;
	static __thread Entry *cur;
	static __thread Entry *next;

	static __thread Entry **hp0;
	static __thread Entry **hp1;
	static __thread Entry **hp2;
	static __thread Entry **hp3;
	static __thread Entry **hp4;
	static __thread Entry **hp5;

	Chunk(int min, int max) :
			MIN(min), MAX(max), counter(0), newChunk(NULL), mergeBuddy(0), nextChunk(
			NULL) {
		if (MAX <= (2 * MIN + 1)) {
			std::cerr << "MAX > (2 * MIN + 1): Condition violated!"
					<< std::endl;
			exit(1);
		}

		entriesArray = new Entry[MAX + 1];		// +1 for dummy header
		for (int i = 0; i <= MAX; i++)
			entriesArray[i] = new Entry();

		head = entriesArray[0];
		mergeBuddy = (mergeBuddy | NO_FREEZE);
	}

	virtual ~Chunk() {
		delete[] entriesArray;
	}

	/*
	 * Checks if swapped bit (LSB) is set in given pointer to a chunk c.
	 */
	static bool isSwapped(Chunk *chunk) {
		return (chunk & SET_SWAPPED_BIT_MASK);
	}

	/*
	 * Returns the value of a pointer c with the swapped bit set to one; it
	 * does not matter if in initial c this bit was set or not.
	 */
	static Chunk *markSwapped(Chunk *chunk) {
		return (chunk | SET_SWAPPED_BIT_MASK);
	}

	/*
	 * Returns the value of a pointer c with the swapped bit set to zero; it
	 * does not matter if in initial c this bit was set or not.
	 */
	static Chunk *clearSwapped(Chunk *chunk) {
		return (chunk & UNSET_SWAPPED_BIT_MASK);
	}

	static Chunk *combineChunkState(Chunk *chunk, FreezeState state) {
		return chunk | state;
	}

	static Chunk *allocate() {
		return new Chunk();
	}

	bool compareAndSetFreezeState(FreezeState oldState, FreezeState newState) {
		long oldMergeBuddy = ((long) getMergeBuddy()) & UNSET_FREEZE_STATE_MASK;
		oldMergeBuddy = ((long) oldMergeBuddy) | oldState;

		long newMergeBuddy = ((long) oldMergeBuddy) | newState;

		return compareAndSetMergeBuddyAndFreezeState(oldMergeBuddy,
				newMergeBuddy);
	}

	int getFreezeState() {
		return (long) mergeBuddy & SET_FREEZE_STATE_MASK;
	}

	long getMergeBuddy() {
		return mergeBuddy;	// & UNSET_FREEZE_STATE_MASK;
	}

	bool compareAndSetMergeBuddy(long oldMergeBuddy, long newMergeBuddy) {
		FreezeState currentState = getFreezeState();
		return compareAndSetMergeBuddyAndFreezeState(
				oldMergeBuddy | currentState, newMergeBuddy | currentState);
	}

	bool compareAndSetMergeBuddyAndFreezeState(long oldMergeBuddy,
			long newMergeBuddy) {
		return mergeBuddy.compare_exchange_strong(oldMergeBuddy, newMergeBuddy);
	}

	bool search(long key, TData data) {
		Chunk *chunk = findChunk(key);
		bool result = searchInChunk(chunk, key, data);
		hp5 = hp4 = hp3 = hp2 = NULL;
		return result;
	}

	bool insert(long key, TData data) {
		Chunk *chunk = findChunk(key);
		bool result = insertToChunk(chunk, key, data);
		hp5 = hp4 = hp3 = hp2 = NULL;
		return result;
	}

	bool deleteChunk(long key, TData data) {
		Chunk *chunk = findChunk(key);
		bool result = deleteInChunk(chunk, key, data);
		hp5 = hp4 = hp3 = hp2 = NULL;
		return result;
	}

	bool searchInChunk(Chunk *chunk, long key, TData data);
	Chunk *findChunk(long key);

	bool insertToChunk(Chunk *chunk, long key, TData data) {
		bool result;
		Entry *current = allocateEntry(chunk, key, data); // Find an available entry

		while (current == NULL) {	// No available entry freeze and try again
			chunk = freeze(chunk, key, data, INSERT, &result);
			if (chunk == NULL)
				return result;		// freeze completed insertion
			current = allocateEntry(chunk, key, data);		// retry allocation
		}

		ReturnCode retCode = insertEntry(chunk, current, key);
		switch (retCode) {
		case SUCCESS_THIS:
			incCount(chunk);
			result = true;
			break;

		case SUCCESS_OTHER:
			result = true;
			break;

		case EXISTED:
			if (clearEntry(chunk, current))
				return false;
			return true;
			break;
		}

		*hp0 = *hp1 = NULL;
		return result;
	}

	bool deleteInChunk(Chunk *chunk, long key, TData data);

	Entry *allocateEntry(Chunk * chunk, long key, TData data) {
		uint128 newKeyData = Utils::combine(key, data);

		// default value
		uint128 expectedKeyData = Utils::combine(Utils::DEFAULT_KEY, 0);

		// look for some empty entry
		for (int i = 0; i < MAX; i++) {
			if (entriesArray[i].keyData == expectedKeyData) {
				uint128 oldKeyData = entriesArray[i].keyData;
				if (oldKeyData
						== Utils::InterlockedCompareExchange128(
								&(entriesArray[i].keyData), oldKeyData,
								newKeyData))
					return entriesArray[i];
			}
		}

		return NULL;
	}

	Chunk *freeze(Chunk *chunk, long key, TData data, TriggerType trigger,
	bool *result) {
		chunk->compareAndSetFreezeState(NO_FREEZE, INTERNAL_FREEZE);

		markChunkFrozen(chunk);
		stabilizeChunk(chunk);

		if (chunk->getFreezeState() == EXTERNAL_FREEZE) {
			Chunk *master = chunk->mergeBuddy;
			Chunk *masterOldMergeBuddy = combineChunkState(NULL,
					INTERNAL_FREEZE);
			Chunk *masterNewBuddy = combineChunkState(chunk, INTERNAL_FREEZE);

			master->compareAndSetMergeBuddyAndFreezeState(masterOldMergeBuddy,
					masterNewBuddy);

			return freezeRecovery(chunk->mergeBuddy, key, data, MERGE, chunk,
					trigger, result);
		}

		RecovType decision = freezeDecision(chunk);
		Chunk *mergePartner = NULL;
		if (decision == MERGE)
			mergePartner = findMergeSlave(chunk);
		return freezeRecovery(chunk, key, data, decision, mergePartner, trigger,
				result);
	}

	ReturnCode insertEntry(Chunk * chunk, Entry *entry, long key) {
		while (true) {
			Entry *savedNext = entry->nextEntry.load();
			// Find insert location and pointers to current and previous entries
			if (find(chunk, key)) {
				if (entry == cur)
					return SUCCESS_OTHER;
				return EXISTED;
			}

			// If neighbourhood is frozen keep it frozen
			if (Entry::isFrozen((uint128) savedNext))
				Entry::markFrozen((uint128) cur);

			if (Entry::isFrozen((uint128) cur))
				Entry::markFrozen((uint128) entry);

			if (!std::atomic_compare_exchange_strong(&(entry->nextEntry),
					(long *) &savedNext, (long) cur))
				continue;

			// TODO: Changed this
			if (!std::atomic_compare_exchange_strong(&((*prev)->nextEntry),
					(long *) &cur, (long) entry))
				continue;

			return SUCCESS_THIS;
		}
	}

	bool clearEntry(Chunk *chunk, Entry *entry);
	void incCount(Chunk *chunk);bool find(Chunk *chunk, long key);

	void markChunkFrozen(Chunk *chunk) {
		for (int i = 0; i < chunk->counter.load(); i++) {
			long savedNext = (uint128) entriesArray[i].nextEntry.load();
			while (!Entry::isFrozen(savedNext)) {
				entriesArray[i].nextEntry.compare_exchange_strong(savedNext,
						(long) Entry::markFrozen(savedNext));
				savedNext = entriesArray[i].nextEntry.load();
			}

			uint128 savedWord = entriesArray[i].keyData;
			while (!Entry::isFrozen(savedWord)) {
				Utils::InterlockedCompareExchange128(&(entriesArray[i].keyData),
						savedWord, Entry::markFrozen(savedWord));
				savedWord = entriesArray[i].keyData;
			}
		}
	}

	void stabilizeChunk(Chunk *chunk);
	RecovType freezeDecision(Chunk *chunk);
	Chunk *findMergeSlave(Chunk *chunk);
	Chunk *freezeRecovery(Chunk *chunk, long key, TData data, RecovType recov,
			Chunk *mergePartner, TriggerType trigger, bool *result);
};

//} /* namespace lflist */

#endif /* CHUNK_HPP_ */
