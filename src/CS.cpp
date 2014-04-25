#include "CS.h"

#include <memory.h>
#include <stdlib.h>
#include <cmath>

#include "NGM.h"
#include "Timing.h"
#include "Debug.h"
#include "AlignmentBuffer.h"

#undef module_name
#define module_name "CS"

static const int cInvalidLocation = -99999999;
static const SequenceLocation sInvalidLocation(cInvalidLocation, 0, false);
//static const LocationScore sInvalidLocationScore(sInvalidLocation, 0, 0); // { sInvalidLocation, {0} };

//#ifdef _DEBUGCS
//FILE* ofp2;
//#endif

volatile int CS::s_ThreadCount = 0;

//long CS::maxHitSum = 0;
//long CS::readCount = 0;

bool up = false;
int kCount = 0;

//int const cThreshhold = 2;

//Default values
int x_SrchTableBitLen = 24;
//Number of bases per batch
int const cBatchSize = 1800000;
//int const cBatchSize =   10000;
uint const cPrefixBaseSkip = 0;

float const cOverflowThresh = 0.1f;
float const cOverflowLimit = 10;

uint CS::prefixBasecount = 13;
uint CS::prefixBits = prefixBasecount * 2;
ulong CS::prefixMask = ((ulong) 1 << prefixBits) - 1;

inline int min(int a, int b) {
	return (a < b) ? a : b;
}

// A->0 C->1 T->2 G->3
inline char encode(char c) {
	return (c >> 1) & 3;
}

void CS::PrefixIteration(char const * sequence, uint length, PrefixIterationFn func, ulong mutateFrom, ulong mutateTo, void* data, uint prefixskip, uint offset, int prefixBaseCount) {
	prefixBasecount = prefixBaseCount;
	prefixBits = prefixBasecount * 2;
	prefixMask = ((ulong) 1 << prefixBits) - 1;

	CS::PrefixIteration(sequence, length, func, mutateFrom, mutateTo, data, prefixskip, offset);

	prefixBasecount = Config.GetInt("kmer", 4, 32);
	prefixBits = prefixBasecount * 2;
	prefixMask = ((ulong) 1 << prefixBits) - 1;
}

// Iteriert ueber jeden Praefix in sequence und fuehrt fuer diesen die Funktion func aus
void CS::PrefixIteration(char const * sequence, uint length, PrefixIterationFn func, ulong mutateFrom, ulong mutateTo, void* data, uint prefixskip, uint offset) {
	if (length < prefixBasecount)
		return;

	if (*sequence == 'N') {
		uint n_skip = 1;
		while (*(sequence + n_skip) == 'N')
			++n_skip;

		sequence += n_skip;

		if (n_skip >= (length - prefixBasecount))
			return;
		length -= n_skip;
		offset += n_skip;
	}

	ulong prefix = 0;
	for (uint i = 0; i < prefixBasecount - 1; ++i) {
		char c = *(sequence + i);
		if (c == 'N') {
			PrefixIteration(sequence + i + 1, length - i - 1, func, mutateFrom, mutateTo, data, prefixskip, offset + i + 1);
			return;
		}

		prefix = prefix << 2;
		char cx = encode(c);
		prefix |= cx;
	}

	uint skipcount = prefixskip;
	for (uint i = prefixBasecount - 1; i < length; ++i) {
		char c = *(sequence + i);
		if (c == 'N') {
			PrefixIteration(sequence + i + 1, length - i - 1, func, mutateFrom, mutateTo, data, prefixskip, offset + i + 1);
			return;
		}

		prefix = prefix << 2;
		char cx = encode(*(sequence + i));
		prefix |= cx;
		prefix &= prefixMask;

		if (skipcount == prefixskip) {
			func(prefix, offset + i + 1 - prefixBasecount, mutateFrom, mutateTo, data);
			skipcount = 0;
		} else {
			++skipcount;
		}
	}
}

// mutate T (0x2) -> C (0x1)
//ulong const mutateFrom = 0x2;
//ulong const mutateTo = 0x1;
//ulong mutateFrom = 0x2;
//ulong mutateTo = 0x1;

void CS::PrefixMutateSearch(ulong prefix, uint pos, ulong mutateFrom, ulong mutateTo, void* data) {
	static int const cMutationLocLimit = Config.GetInt("bs_cutoff");
	ulong const mask = 0x3;

	int mutationLocs = 0;
	for (int i = 0; i < (int) prefixBasecount; ++i) {
		ulong base = mask & (prefix >> (i * 2));
		if (base == mutateFrom)
			++mutationLocs;
	}

	if (mutationLocs <= cMutationLocLimit)
		PrefixMutateSearchEx(prefix, pos, mutateFrom, mutateTo, data);
}

void CS::PrefixMutateSearchEx(ulong prefix, uint pos, ulong mutateFrom, ulong mutateTo, void* data, int mpos) {
	PrefixSearch(prefix, pos, mutateFrom, mutateTo, data);

	ulong const mask = 0x3;
	for (int i = mpos; i < (int) prefixBasecount; ++i) {
		ulong cur = mask & (prefix >> (i * 2));

		if (cur == mutateFrom) {
			ulong p1 = (prefix & ~(mask << (i * 2)));
			ulong p2 = (mutateTo << (i * 2));
			PrefixMutateSearchEx(p1 | p2, pos, mutateFrom, mutateTo, data, i + 1);
		}
	}
}

void CS::PrefixSearch(ulong prefix, uint pos, ulong mutateFrom, ulong mutateTo, void* data) {
	CS * cs = (CS*) data;

	RefEntry const * cur = cs->m_RefProvider->GetRefEntry(prefix, cs->m_entry); // Liefert eine liste aller Vorkommen dieses Praefixes in der Referenz

	int const readLength = cs->m_CurrentReadLength;

	if (cur != 0 && cur->refTotal == 0) {
		kCount += 1;
	}

	while (cur != 0) {
		//Get kmer-weight.
		//float weight = cur->weight / 100.0f;
		//cs->weightSum += cur->weight;
		float weight = 1.0f;

		uint correction = (cur->reverse) ? (readLength - (pos + CS::prefixBasecount)) : pos;

//#ifdef _DEBUGCSVERBOSE
//		Log.Message("Qry Seq %i - Prefix 0x%x got %i locs (sum %i)", cs->m_CurrentSeq, prefix, cur->refTotal);
//#endif

		int const n = cur->refCount;
		for (int i = 0; i < n; ++i) {
			cs->AddLocationStd(cs->GetBin(cur->ref[i].m_Location - correction), cur->reverse, weight);
		}

		cur = cur->nextEntry;
	}
}

void CS::AddLocationStd(uint const m_Location, bool const reverse, double const freq) {
	uint hpoc = c_SrchTableLen;
	uint l = (uint) c_SrchTableLen;
	bool newEntry = false;

	uint hpo = Hash(m_Location);
	while ((newEntry = (rTable[hpo].state & 0x7FFFFFFF) == currentState) && !(rTable[hpo].m_Location == m_Location)) {
		++hpo;
		if (hpo >= l)
			hpo = 0;
		if (--hpoc == 0) {
			throw 1;
		}
	}

	float score = freq;
	if (!newEntry) {
		rTable[hpo].m_Location = m_Location;
		rTable[hpo].state = currentState & 0x7FFFFFFF;
		if (reverse) {
			rTable[hpo].fScore = 0.0f;
			rTable[hpo].rScore = score;
		} else {
			rTable[hpo].fScore = score;
			rTable[hpo].rScore = 0.0f;
		}
	} else {
		//Add kmer-weight to position
		if (reverse) {
			score = (rTable[hpo].rScore += freq);
		} else {
			score = (rTable[hpo].fScore += freq);
		}
	}

	//Compute max k-mer weight for this read
	if (score > maxHitNumber) {
		maxHitNumber = score;
		//currentThresh = round(maxHitNumber * m_CsSensitivity);
		currentThresh = maxHitNumber * m_CsSensitivity;
	}

	//If kmer-weight larger than threshold -> add to rList.
	if (!(rTable[hpo].state & 0x80000000) && score >= currentThresh) {
		rTable[hpo].state |= 0x80000000;
		rList[rListLength++] = hpo;
	}

}

void CS::AddLocationFallback(SequenceLocation const & loc, double const freq) {
	throw "Not implemented (AddLocationFallback)";
}

#include <stdio.h>
#include <sstream>

void CS::debugCS(MappedRead * read, int& n, float& mi_Threshhold) {

	int count = 0;
	int accepted = 0;
	float max = 0.0f;
	for (int i = 0; i < c_SrchTableLen; ++i) {

		//Log.Message("%u %u", rTable[i].state, currentState);
		if ((rTable[i].state & 0x7FFFFFFF) == currentState) {

			max = std::max(std::max(max, rTable[i].fScore), rTable[i].rScore);
			//Correct position
			SequenceLocation loc;
			loc.m_Location = ResolveBin(rTable[i].m_Location);
			SequenceProvider.convert(loc);

			int refNameLength = 0;
			if (rTable[i].fScore > 0.0f) {
				if(rTable[i].fScore >= mi_Threshhold) {
					Log.Debug(8192, "READ_%d\tCS_RESULTS\tInternal location: %u (+), Location: %u (Ref: %s), Score: %f (ACCEPT)", read->ReadId, rTable[i].m_Location, loc.m_Location, SequenceProvider.GetRefName(loc.getrefId(), refNameLength), rTable[i].fScore);
					accepted += 1;
				} else {
					Log.Debug(4096, "READ_%d\tCS_DETAILS\tInternal location: %u (+), Location: %u (Ref: %s), Score: %f (REJECT)", read->ReadId, rTable[i].m_Location, loc.m_Location, SequenceProvider.GetRefName(loc.getrefId(), refNameLength), rTable[i].fScore);
				}
				count += 1;
			}
			if (rTable[i].rScore > 0.0f) {
				if(rTable[i].rScore >= mi_Threshhold) {
					Log.Debug(8192, "READ_%d\tCS_RESULTS\tInternal location: %u (-), Location: %u (Ref: %s), Score: %f (ACCEPT)", read->ReadId, rTable[i].m_Location, loc.m_Location, SequenceProvider.GetRefName(loc.getrefId(), refNameLength), rTable[i].rScore);
					accepted += 1;
				} else {
					Log.Debug(4096, "READ_%d\tCS_DETAILS\tInternal location: %u (-), Location: %u (Ref: %s), Score: %f (REJECT)", read->ReadId, rTable[i].m_Location, loc.m_Location, SequenceProvider.GetRefName(loc.getrefId(), refNameLength), rTable[i].rScore);
				}
				count += 1;
			}
		}
	}

	Log.Debug(8, "READ_%d\tCS\tSummary: %d CMRs (%d without filter), %d accepted, theta: %f, max: %f", read->ReadId, n, count, accepted, mi_Threshhold, max);

}

//#ifdef _DEBUGCS
//#include <stdio.h>
//#include <sstream>

//void CS::debugCS(MappedRead * read, int& n, float& mi_Threshhold) {
//	FILE* ofp;
//	std::stringstream str;
//	str << "kmers-" << read->name << ".csv";
//	ofp = fopen(str.str().c_str(), "w");
//
//	FILE* ofpBed;
//	std::stringstream strBed;
//	strBed << "cmrs-" << read->name << ".bed";
//	ofpBed = fopen(strBed.str().c_str(), "w");
//
//	int count = 0;
//	float max = 0.0f;
//	for (int i = 0; i < c_SrchTableLen; ++i) {
//
//		//Log.Message("%u %u", rTable[i].state, currentState);
//		if ((rTable[i].state & 0x7FFFFFFF) == currentState) {
//
//			max = std::max(std::max(max, rTable[i].fScore), rTable[i].rScore);
//			//Correct position
//			int refCount = SequenceProvider.GetRefCount();
//			uint m_Location = ResolveBin(rTable[i].m_Location);
//			int j = 0;
//			while (j < refCount && m_Location >= SequenceProvider.GetRefStart(j)) {
//				j += (NGM.DualStrand()) ? 2 : 1;
//			}
//			if (j == 0) {
//				//Log.Message("%s (%d) - %s: %s, %s", read->name, read->ReadId, read->Seq, read->Buffer1, read->Buffer2);
//				//Log.Error("Couldn't resolve mapping position: %u!", m_Location);
//				//Fatal();
//			} else {
//				j -= (NGM.DualStrand()) ? 2 : 1;
//
//				m_Location -= SequenceProvider.GetRefStart(j);
//			}
//			int m_RefId = j;
//			int refNameLength = 0;
//			if (rTable[i].fScore > 0.0f) {
//				if(rTable[i].fScore >= mi_Threshhold) {
//#ifdef _DEBUGCSVERBOSE
//					Log.Green("%s - Loc: %u (+), Location: %u (Ref: %s), Score: %f", read->name, rTable[i].m_Location, m_Location, SequenceProvider.GetRefName(m_RefId, refNameLength), rTable[i].fScore);
//					fprintf(ofpBed, "%s\t%d\t%d\t%s\t%f\t+\n", SequenceProvider.GetRefName(m_RefId, refNameLength), m_Location, m_Location + 12, read->name, rTable[i].fScore);
//#endif
//				} else {
//#ifdef _DEBUGCSVERBOSE
//					//Log.Message("%s - Loc: %u (+), Location: %u (Ref: %s), Score: %f", read->name, rTable[i].m_Location, m_Location, SequenceProvider.GetRefName(m_RefId, refNameLength), rTable[i].fScore);
//#endif
//				}
//#ifdef _DEBUGCSKMERS
//				fprintf(ofp, "%d\t%d\t%u\t%u\t%i\t%f\n", read->ReadId, 1, rTable[i].m_Location, m_Location, m_RefId, rTable[i].fScore);
//#endif
//				count += 1;
//			}
//			if (rTable[i].rScore > 0.0f) {
//				if(rTable[i].rScore >= mi_Threshhold) {
//#ifdef _DEBUGCSVERBOSE
//					Log.Green("%s - Loc: %u (+), Location: %u (Ref: %s), Score: %f", read->name, rTable[i].m_Location, m_Location, SequenceProvider.GetRefName(m_RefId, refNameLength), rTable[i].rScore);
//					fprintf(ofpBed, "%s\t%d\t%d\t%s\t%f\t-\n", SequenceProvider.GetRefName(m_RefId, refNameLength), m_Location, m_Location + 12, read->name, rTable[i].rScore);
//#endif
//				} else {
//#ifdef _DEBUGCSVERBOSE
//					//Log.Message("%s - Loc: %u (-), Location: %u (Ref: %s), Score: %f", read->name, rTable[i].m_Location, m_Location, SequenceProvider.GetRefName(m_RefId, refNameLength), rTable[i].rScore);
//#endif
//				}
//#ifdef _DEBUGCSKMERS
//				fprintf(ofp, "%d\t%d\t%u\t%u\t%i\t%f\n", read->ReadId, -1, rTable[i].m_Location, m_Location, m_RefId, rTable[i].rScore);
//#endif
//				count += 1;
//			}
//		}
//	}
//	fclose(ofp);
//	fclose(ofpBed);
//#ifdef _DEBUGCSVERBOSE
//	Log.Green("Read %s: %d CMRs, %d before filtering, theta: %f, max: %f", read->name, count, n, mi_Threshhold, max);
//#endif
//	//Log.Green("max = %f", max);
//	//Log.Green("n = %d", n);
//	//Log.Green("c = %d", count);
//	//Log.Green("t = %f", mi_Threshhold);
//	fprintf(ofp2, "%s\t%f\t%d\t%d\t%f\n", read->name, max, n, count, mi_Threshhold);
//
//}
//#endif

int CS::CollectResultsStd(MappedRead * read) {

	static float const mink = Config.GetFloat("kmer_min");

	int max = (read->length - CS::prefixBasecount + 1) * 0.9;

	if (kCount > max)
		read->mappingQlty = 0;

	read->s = maxHitNumber;

	float mi_Threshhold = std::max(mink, currentThresh);

	int n = rListLength;

	debugCS(read, n, mi_Threshhold);

	int index = 0;
	if (2 * n > tmpSize) {
		tmpSize = 2 * n * 1.5f;
		Log.Debug(LOG_CS_DETAILS, "Increasing size of location buffer to %d", tmpSize);
		delete[] tmp;
		tmp = new LocationScore[tmpSize];
	}

	for (int i = 0; i < n; ++i) {
		CSTableEntry temp = rTable[rList[i]];

		if (temp.fScore >= mi_Threshhold) {
			LocationScore * toInsert = &tmp[index++];
			toInsert->Score.f = temp.fScore;
			toInsert->Location.m_Location = ResolveBin(temp.m_Location);
			toInsert->Location.setReverse(false);
		}
		if (temp.rScore >= mi_Threshhold) {
			LocationScore * toInsert = &tmp[index++];
			toInsert->Score.f = temp.rScore;
			toInsert->Location.m_Location = ResolveBin(temp.m_Location);
			toInsert->Location.setReverse(true);
		}
	}
	static int const maxScores = Config.GetInt("max_cmrs");
	if (index < maxScores)
		read->AllocScores(tmp, index);

	return index;
}

int CS::CollectResultsFallback(MappedRead * read) {
	throw "Not implemented (CollectResultsFallback)";
	return 0;
}

void CS::SendToBuffer(MappedRead * read, ScoreBuffer * sw, AlignmentBuffer * out) {
	if (read == 0)
		return;

	int count = read->numScores();

	if (count == 0) {
		read->Calculated = 0;
		out->addRead(read, -1);
	} else {
		read->Calculated = 0;
		sw->addRead(read, count);
		++m_WrittenReads;
	}
}

void CS::Cleanup() {
	NGM.ReleaseWriter();
}

int CS::RunBatch(ScoreBuffer * sw, AlignmentBuffer * out) {
	PrefixIterationFn pFunc = &CS::PrefixSearch;
	if (m_EnableBS)
		pFunc = &CS::PrefixMutateSearch;

	int nScoresSum = 0;

	for (size_t i = 0; i < m_CurrentBatch.size(); ++i) {

		m_CurrentSeq = m_CurrentBatch[i]->ReadId;
//		//TODO: remove
//		if(strcmp("HWUSI-EAS475:1:12:17529:18194#0/1", m_CurrentBatch[i]->name) == 0) {
//			Log.Error("Read %s: candidate search %d", m_CurrentBatch[i]->name, m_TID);
//			getchar();
//		}
		currentState = 2 * i;
		rListLength = 0;
		maxHitNumber = 0.0f;
		currentThresh = 0.0f;
		//weightSum = 0.0f;
		kCount = 0;

		ulong mutateFrom;
		ulong mutateTo;
		static bool const isPaired = Config.GetInt("paired") > 0;
		if (isPaired && (m_CurrentBatch[i]->ReadId & 1)) {
			//Second mate
			mutateFrom = 0x0;
			mutateTo = 0x3;
		} else {
			//First mate
			mutateFrom = 0x2;
			mutateTo = 0x1;
		}

		m_CurrentReadLength = m_CurrentBatch[i]->length;

		++m_ProcessedReads;
		bool fallback = m_Fallback;

		if (m_CSThreadID < NGMStats::cMaxThreadStats)
			NGM.Stats->CS[m_CSThreadID].CurrentRead = m_CurrentSeq;

		char const * const qrySeq = m_CurrentBatch[i]->Seq;
		int qryLen = m_CurrentBatch[i]->length;

		if (!fallback) {
			try {
				PrefixIteration(qrySeq, qryLen, pFunc, mutateFrom, mutateTo, this, m_PrefixBaseSkip);
				nScoresSum += CollectResultsStd(m_CurrentBatch[i]);
			} catch (int overflow) {
//				Log.Verbose("Interrupt on read %i: %i candidates in %i slots filled by %i prefixes containing %i locs",
//						m_CurrentSeq,
//						m_Candidates,
//						m_UsedSlots,
//						m_TotalPrefixes,
//						m_TotalLocs);

				++m_Overflows;

				// Initiate fallback search on iTable
				fallback = true;
			}
		}
		//Log.Message("CCount %u", cCount);
		if (fallback) {
			int c_SrchTableBitLenBackup = c_SrchTableBitLen;
			int x = 2;
			while (fallback && (c_SrchTableBitLenBackup + x) <= 20) {
				fallback = false;
				try {

					c_SrchTableBitLen = c_SrchTableBitLenBackup + x;
					c_BitShift = 32 - c_SrchTableBitLen;
					c_SrchTableLen = (int) pow(2, c_SrchTableBitLen);

					rListLength = 0;
					currentState = 2 * i + 1;
					maxHitNumber = 0.0f;
					currentThresh = 0.0f;

					PrefixIteration(qrySeq, qryLen, pFunc, mutateFrom, mutateTo, this, m_PrefixBaseSkip);
					nScoresSum += CollectResultsStd(m_CurrentBatch[i]);

				} catch (int overflow) {
					fallback = true;
					x += 1;
				}
			}
			c_SrchTableBitLen = c_SrchTableBitLenBackup;
			c_BitShift = 32 - c_SrchTableBitLen;
			c_SrchTableLen = (int) pow(2, c_SrchTableBitLen);
		}

//#ifdef _DEBUGCS
//		Log.Warning("You are in DEBUGCS mode. Program quits after CMR search.");
//		NGM.AddMappedRead(m_CurrentBatch[i]->ReadId);
//		delete m_CurrentBatch[i];
//		m_CurrentBatch[i] = 0;
//#else
		SendToBuffer(m_CurrentBatch[i], sw, out);
//#endif
	}

	return nScoresSum;
}

void CS::DoRun() {

	IAlignment * oclAligner;
	int gpu = m_TID;
	if (Config.Exists("gpu")) {
		int threadcount = 1;
		static const int cMaxAligner = 32;
		threadcount = Config.GetInt("gpu", 1, cMaxAligner);
		int * gpus = new int[threadcount];
		if (Config.Exists("gpu")) {
			Config.GetIntArray("gpu", gpus, threadcount);
		} else {
			gpus[0] = 0;
		}
		gpu = gpus[m_TID % threadcount];
	}

	NGM.AquireOutputLock();
	oclAligner = NGM.CreateAlignment(gpu | (std::min(Config.GetInt("format", 0, 2), 1) << 8));
	AlignmentBuffer * alignmentBuffer = new AlignmentBuffer(Config.GetString("output"), oclAligner);
	ScoreBuffer * scoreBuffer = new ScoreBuffer(oclAligner, alignmentBuffer);
	NGM.ReleaseOutputLock();

	int x_SrchTableLen = (int) pow(2, x_SrchTableBitLen);

	rTable = new CSTableEntry[x_SrchTableLen];
	Log.Debug(LOG_CS_DETAILS, "Sizeof CSTableEntry %d (%d)", sizeof(CSTableEntry), sizeof(SequenceLocation));
	Log.Debug(LOG_CS_DETAILS, "rTable: %d (%d x (%d + %d))", (sizeof(CSTableEntry) + sizeof(int)) * x_SrchTableLen, x_SrchTableLen, sizeof(CSTableEntry), sizeof(int));
	rList = new int[x_SrchTableLen];

	for (int i = 0; i < x_SrchTableLen; ++i) {
		rTable[i].m_Location = sInvalidLocation.m_Location;
		rTable[i].state = -1;
		rList[i] = -1;
	}

	m_CsSensitivity = 0.50f;
	if (Config.Exists("sensitivity")) {
		m_CsSensitivity = Config.GetFloat("sensitivity", 0, 1);
	} else {
		Log.Warning("Sensitivity parameter neither set nor estimated. Falling back to default.");
	}

	m_RefProvider = 0;

	Timer tmr;
	m_RefProvider = NGM.GetRefProvider(m_TID);
	while (NGM.ThreadActive(m_TID, GetStage()) && ((m_CurrentBatch = NGM.GetNextReadBatch(m_BatchSize)), (m_CurrentBatch.size() > 0))) {
		Log.Debug(LOG_CS_DETAILS, "CS Thread %i got batch (len %i)", m_TID, m_CurrentBatch.size());
		tmr.ST();

		int nCRMsSum = RunBatch(scoreBuffer, alignmentBuffer);
		scoreBuffer->flush();

		float elapsed = tmr.ET();
		Log.Debug(LOG_CS_DETAILS, "CS Thread %i finished batch (len %i) with %i overflows, length %d (elapsed: %.3fs)", m_TID, m_CurrentBatch.size(), m_Overflows, c_SrchTableBitLen, elapsed);

		NGM.Stats->readsPerSecond = (NGM.Stats->csTime + 1.0f / (elapsed / m_CurrentBatch.size())) / 2.0f;

		NGM.Stats->alignTime = std::max(0.0f, alignmentBuffer->getTime());
		NGM.Stats->scoreTime = std::max(0.0f, scoreBuffer->getTime() - NGM.Stats->alignTime);
		NGM.Stats->csTime = std::max(0.0f, elapsed - NGM.Stats->scoreTime - NGM.Stats->alignTime);

		NGM.Stats->csLength = c_SrchTableBitLen;
		NGM.Stats->csOverflows = m_Overflows;
		NGM.Stats->avgnCRMS = nCRMsSum / m_CurrentBatch.size();

		if (!Config.Exists("search_table_length")) {
			if (m_Overflows <= 5 && !up && c_SrchTableBitLen > 8) {
				c_SrchTableBitLen -= 1;
				c_BitShift = 32 - c_SrchTableBitLen;
				c_SrchTableLen = (int) pow(2, c_SrchTableBitLen);
//			Log.Warning("Overflow: Switching to %d bits (%d, %d)", c_SrchTableBitLen, c_BitShift, c_SrchTableLen);
			} else if (m_Overflows > m_BatchSize * 0.01f) {
				c_SrchTableBitLen += 1;
				c_BitShift = 32 - c_SrchTableBitLen;
				c_SrchTableLen = (int) pow(2, c_SrchTableBitLen);
				up = true;
//			Log.Warning("Overflow: Switching to %d bits (%d, %d)", c_SrchTableBitLen, c_BitShift, c_SrchTableLen);
			}
		}
		m_Overflows = 0;
	}

	delete scoreBuffer; scoreBuffer = 0;
	delete alignmentBuffer; alignmentBuffer = 0;
	NGM.DeleteAlignment(oclAligner);
	Log.Debug(LOG_CS_DETAILS, "CS Thread %i finished (%i reads processed, %i reads written, %i reads discarded)", m_TID, m_ProcessedReads, m_WrittenReads, m_DiscardedReads);
}
void CS::Init() {
	prefixBasecount = Config.GetInt("kmer", 4, 32);
	prefixBits = prefixBasecount * 2;
	prefixMask = ((ulong) 1 << prefixBits) - 1;

	bool m_EnableBS = (Config.GetInt("bs_mapping", 0, 1) == 1);
	if (m_EnableBS) {
		static int const cMutationLocLimit = Config.Exists("bs_cutoff") ? Config.GetInt("bs_cutoff") : 6;
		Log.Message("BS mapping enabled. Max. number of A/T per k-mer set to %d", cMutationLocLimit);
	}
}

CS::CS(bool useBuffer) :
		m_CSThreadID((useBuffer) ? (AtomicInc(&s_ThreadCount) - 1) : -1), m_BatchSize(cBatchSize / Config.GetInt("qry_avg_len")), m_ProcessedReads(
				0), m_WrittenReads(0), m_DiscardedReads(0), m_EnableBS(false), m_Overflows(0), m_entry(new RefEntry(0)), c_SrchTableBitLen(
				Config.Exists("search_table_length") ? Config.GetInt("search_table_length") : 16), c_BitShift(32 - c_SrchTableBitLen), c_SrchTableLen(
				(int) pow(2, c_SrchTableBitLen)), m_PrefixBaseSkip(0), m_Fallback((c_SrchTableLen <= 0)) // cTableLen <= 0 means always use fallback
{

	m_EnableBS = (Config.GetInt("bs_mapping", 0, 1) == 1);

	if (m_EnableBS) {
		m_PrefixBaseSkip = (Config.Exists("kmer_skip")) ? Config.GetInt("kmer_skip", 0, -1) : cPrefixBaseSkip;
	}

	rTable = 0;

	Log.Debug(LOG_CS_DETAILS, "SearchTabLen: %d (%d)", c_SrchTableLen, c_SrchTableBitLen);

#ifdef _DEBUGCS
	ofp2 = fopen("cs-results.txt", "w");
#endif

	currentState = 0;
	m_entry->nextEntry = new RefEntry(0);
	tmpSize = 10000;
	tmp = new LocationScore[tmpSize];

}

CS::~CS() {
#ifdef _DEBUGCS
	fclose(ofp2);
#endif
	if (tmp != 0) {
		delete[] tmp;
		tmp = 0;
	}
	if (rTable != 0)
		delete[] rTable;

	if (rList != 0) {
		delete[] rList;
	}
}

//void CS::CheckRTable() {
//	for (int i = 0; i < c_SrchTableLen; ++i) {
//		if (rTable[i].Location.m_Location != cInvalidLocation) {
//			Log.Warning("Unclean rTable (qry = %i, elem = %i): loc = %i", m_CurrentSeq, i, rTable[i].Location.m_Location);
//			rTable[i] = sInvalidLocationScore;
//		}
//	}
//}

void CS::CheckFallback() {
//	static float sOverflowThresh =
//			(Config.Exists("cs_overflowthresh")) ?
//					Config.GetFloat("cs_overflowthresh", 0.0f, 1.0f) :
//					cOverflowThresh;
//	static int sOverflowLimit =
//			(Config.Exists("cs_overflowlimit")) ?
//					Config.GetInt("cs_overflowlimit") : cOverflowLimit;
//
//	if (sOverflowLimit <= 0) {
//		m_Fallback = true;
//	}
//
//	if (m_Overflows < sOverflowLimit)
//		return;
//
//	float overflowRatio = (float) m_Overflows / (float) m_ProcessedReads;
//	if (overflowRatio > sOverflowThresh) {
//		m_Fallback = true;
//		Log.Warning("Permanently switching to fallback search (%i out of %i reads overflowed)", m_Overflows, m_ProcessedReads);
//	}
}

