//Query Pipebase without cache, just thread buffer 
#include<iostream>
#include<vector>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<fstream>
#include <sstream>  
#include <string> 
#include <algorithm>
#include<numeric>
#include<sys/time.h>
#include<limits.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h> 
#include<string.h>
#include <sys/stat.h>
#include <omp.h>
#include<unordered_map>
#include<stdio.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<unistd.h>
#include<stdlib.h>
#include<errno.h>
#include<sys/types.h>
#include<fcntl.h>
#include <libaio.h>

#include <succinct/mapper.hpp>
#include"bm25.hpp"
#include"wand_data.hpp"
#include"Mycodec.hpp"


using namespace std;

vector<int64_t>List_offset;
vector<int64_t>Head_offset;
string indexFileName = "";
vector<vector<double>>query_Times;
const uint64_t DISK_BLOCK = 4096;
vector<uint8_t>Head_Data;

vector<uint64_t>Block_Start;
vector<uint32_t>Block_Docid;
vector<float>Block_Max_Term_Weight;

typedef uint32_t term_id_type;
typedef std::vector<term_id_type> term_id_vec;
vector<term_id_vec> queries;
unsigned num_docs = 0;
int IndexFile;

struct AIOReadInfo
{
	int64_t readlength;
	int64_t readoffset;
	int64_t memoffset;
	int64_t listlength;
	uint32_t tid;
	int64_t curSendpos;
	int64_t curReadpos;
};
const int64_t READ_BLOCK = 64 * 1024;
const int AIOLIST_SIZE = 16;
int threadCount = 4;
const unsigned MAXREQUEST = 128;

uint8_t** list_data;
vector<vector<AIOReadInfo>>AIOreadinfo;
vector<io_context_t> ctx;
vector<vector<struct io_event>> events;
vector<vector<struct iocb>> readrequest;
vector<vector<struct iocb*>> listrequest;
vector<int>curAIOLIST_SIZE;
vector<int64_t> curReadID;
void initThreadStruct()
{
	query_Times.resize(threadCount);
	list_data = new uint8_t*[threadCount];
	for (int i = 0; i < threadCount; i++)
	{
		posix_memalign((void**)&list_data[i], DISK_BLOCK, 512 * 1024 * 1024);
	}

	AIOreadinfo.resize(threadCount);
	ctx.resize(threadCount);
	events.resize(threadCount, vector<struct io_event>(MAXREQUEST));
	readrequest.resize(threadCount, vector<struct iocb>(MAXREQUEST));
	listrequest.resize(threadCount, vector<struct iocb*>(MAXREQUEST));
	curReadID.resize(threadCount);
	curAIOLIST_SIZE.resize(threadCount);
	for (int i = 0; i < threadCount; i++)
	{
		ctx[i] = 0;
		if (io_setup(MAXREQUEST, &ctx[i])) {
			perror("io_setup");
			return;
		}
	}
}
bool hasReadFinish()
{
	int threadid = omp_get_thread_num();
	unsigned count = 0;
	for (auto l : AIOreadinfo[threadid])
	{
		if (l.listlength <= l.curSendpos)
			count++;
	}
	return count == AIOreadinfo[threadid].size();
}
void aioReadBlock()
{
	int threadid = omp_get_thread_num();
	while (io_getevents(ctx[threadid], curAIOLIST_SIZE[threadid], MAXREQUEST, events[threadid].data(), NULL) != curAIOLIST_SIZE[threadid])
	{
		;
	}
	for (unsigned i = 0; i < AIOreadinfo[threadid].size(); i++)
	{
		AIOreadinfo[threadid][i].curReadpos = AIOreadinfo[threadid][i].curSendpos;
	}
	int64_t cur = 0, i = 0;
	for (i = 0, cur = 0; cur < AIOLIST_SIZE; curReadID[threadid]++, i++)
	{
		unsigned lid = curReadID[threadid] % AIOreadinfo[threadid].size();
		if (AIOreadinfo[threadid][lid].listlength <= AIOreadinfo[threadid][lid].curSendpos)
		{
			if (hasReadFinish())break;
			else continue;
		}
		io_prep_pread(&readrequest[threadid][cur], IndexFile, list_data[threadid] + AIOreadinfo[threadid][lid].memoffset, AIOreadinfo[threadid][lid].readlength, AIOreadinfo[threadid][lid].readoffset);
		listrequest[threadid][cur] = &readrequest[threadid][cur];
		AIOreadinfo[threadid][lid].memoffset += AIOreadinfo[threadid][lid].readlength;
		AIOreadinfo[threadid][lid].readoffset += AIOreadinfo[threadid][lid].readlength;
		AIOreadinfo[threadid][lid].curSendpos += AIOreadinfo[threadid][lid].readlength;
		cur++;
	}
	curAIOLIST_SIZE[threadid] = cur;
	if (cur == 0){ return; }
	if (io_submit(ctx[threadid], curAIOLIST_SIZE[threadid], listrequest[threadid].data()) != curAIOLIST_SIZE[threadid]) {
		perror("io_submit"); 
	}

}
struct block_posting_list {


	class document_enumerator {
	public:
		uint32_t m_n;
		uint8_t const* m_base;
		uint32_t m_blocks;
		uint8_t const* m_block_maxs;
		uint8_t const* m_block_endpoints;
		uint8_t const* m_blocks_data;
		uint64_t m_universe;

		uint32_t m_cur_block;
		uint32_t m_pos_in_block;
		uint32_t m_cur_block_max;
		uint32_t m_cur_block_size;
		uint32_t m_cur_docid;
		SIMDNewPfor m_codec;
		uint32_t m_termid;
		int64_t m_lid;
		int m_threadid;

		uint8_t const* m_freqs_block_data;
		bool m_freqs_decoded;

		std::vector<uint32_t> m_docs_buf;
		std::vector<uint32_t> m_freqs_buf;

		uint32_t block_max(uint32_t block) const
		{
			return ((uint32_t const*)m_block_maxs)[block];
		}
		void QS_NOINLINE decode_docs_block(uint64_t block)
		{
			int64_t tmpendpoint = 0;
			if (block == m_blocks - 1)
			{
				tmpendpoint = List_offset[m_termid + 1] - List_offset[m_termid];
			}
			else
			{
				tmpendpoint = ((uint32_t const*)m_block_endpoints)[block];
			}
#pragma omp flush(AIOreadinfo)
			while (AIOreadinfo[m_threadid][m_lid].curReadpos < tmpendpoint)
			{
				aioReadBlock();
			}
			static const uint64_t block_size = Codec::block_size;
			uint32_t endpoint = block
				? ((uint32_t const*)m_block_endpoints)[block - 1]
				: 0;
			uint8_t const* block_data = m_blocks_data + endpoint;
			m_cur_block_size =
				((block + 1) * block_size <= size())
				? block_size : (size() % block_size);
			uint32_t cur_base = (block ? block_max(block - 1) : uint32_t(-1)) + 1;
			m_cur_block_max = block_max(block);
			m_freqs_block_data =
				m_codec.decode(block_data, m_docs_buf.data(),
				m_cur_block_max - cur_base - (m_cur_block_size - 1),
				m_cur_block_size);

			m_docs_buf[0] += cur_base;

			m_cur_block = block;
			m_pos_in_block = 0;
			m_cur_docid = m_docs_buf[0];
			m_freqs_decoded = false;
		}
		void QS_NOINLINE decode_freqs_block()
		{
			m_codec.decode(m_freqs_block_data, m_freqs_buf.data(),
				uint32_t(-1), m_cur_block_size);
			m_freqs_decoded = true;
		}
		void reset()
		{
			decode_docs_block(0);
		}
		document_enumerator(uint8_t const* headdata, uint8_t const* listdata, uint64_t universe, uint32_t tid, int64_t lid)
			: m_n(0) // just to silence warnings
			, m_base(TightVariableByte::decode(headdata, &m_n, 1))
			, m_blocks(ceil((double)m_n / (double)Codec::block_size))
			, m_block_maxs(m_base)
			, m_block_endpoints(m_block_maxs + 4 * m_blocks)
			, m_blocks_data(listdata)
			, m_universe(universe)
			, m_termid(tid)
			, m_lid(lid)
		{
			m_docs_buf.resize(Codec::block_size);
			m_freqs_buf.resize(Codec::block_size);
			m_threadid = omp_get_thread_num();
		}
		void inline next()
		{
			++m_pos_in_block;
			if (QS_UNLIKELY(m_pos_in_block == m_cur_block_size)) {
				if (m_cur_block + 1 == m_blocks) {
					m_cur_docid = m_universe;
					return;
				}
				decode_docs_block(m_cur_block + 1);
			}
			else {
				m_cur_docid += m_docs_buf[m_pos_in_block] + 1;
			}
		}
		uint64_t docid() const
		{
			return m_cur_docid;
		}
		uint64_t inline freq()
		{
			if (!m_freqs_decoded) {
				decode_freqs_block();
			}
			return m_freqs_buf[m_pos_in_block] + 1;
		}

		uint64_t position() const
		{
			return m_cur_block * Codec::block_size + m_pos_in_block;
		}

		uint64_t size() const
		{
			return m_n;
		}
		void  inline next_geq(uint64_t lower_bound)
		{
			if (QS_UNLIKELY(lower_bound > m_cur_block_max)) {
				// binary search seems to perform worse here
				if (lower_bound > block_max(m_blocks - 1)) {
					m_cur_docid = m_universe;
					return;
				}

				uint64_t block = m_cur_block + 1;
				while (block_max(block) < lower_bound) {
					++block;
				}

				decode_docs_block(block);
			}

			while (docid() < lower_bound) {
				m_cur_docid += m_docs_buf[++m_pos_in_block] + 1;
				assert(m_pos_in_block < m_cur_block_size);
			}
		}

		void  inline move(uint64_t pos)
		{
			assert(pos >= position());
			uint64_t block = pos / Codec::block_size;
			if (QS_UNLIKELY(block != m_cur_block)) {
				decode_docs_block(block);
			}
			while (position() < pos) {
				m_cur_docid += m_docs_buf[++m_pos_in_block] + 1;
			}
		}

	};

};

class wand_data_enumerator{
public:

	wand_data_enumerator(uint64_t _block_start, uint64_t _block_number, vector<float> const & max_term_weight,
		vector<uint32_t> const & block_docid) :
		cur_pos(0),
		block_start(_block_start),
		block_number(_block_number),
		m_block_max_term_weight(max_term_weight),
		m_block_docid(block_docid)
	{}


	void  next_geq(uint64_t lower_bound) {
		while (cur_pos + 1 < block_number &&
			m_block_docid[block_start + cur_pos] <
			lower_bound) {
			cur_pos++;
		}
	}


	float score() const {
		return m_block_max_term_weight[block_start + cur_pos];
	}

	uint64_t docid() const {
		return m_block_docid[block_start + cur_pos];
	}


	uint64_t find_next_skip() {
		return m_block_docid[cur_pos + block_start];
	}

private:
	uint64_t cur_pos;
	uint64_t block_start;
	uint64_t block_number;
	vector<float> const &m_block_max_term_weight;
	vector<uint32_t> const &m_block_docid;

};

void read_Head_Length(string filename)
{
	FILE *lengthfile = fopen((filename + ".head-l").c_str(), "rb");
	unsigned tmplength = 0;
	int64_t prelength = 0;
	fread(&num_docs, sizeof(unsigned), 1, lengthfile);
	cout << "num_docs=" << num_docs << endl;
	while (fread(&tmplength, sizeof(unsigned), 1, lengthfile))
	{
		Head_offset.push_back(prelength);
		prelength += tmplength;
	}
	Head_offset.push_back(prelength);
	cout << "All we have " << Head_offset.size() - 1 << " heads" << endl;
	fclose(lengthfile);
}
void read_Head_Data(string filename)
{
	filename = filename + ".head";
	FILE *file = fopen(filename.c_str(), "rb");
	cout << "Head Data has " << Head_offset[Head_offset.size() - 1] << " Bytes" << endl;
	int64_t length = Head_offset[Head_offset.size() - 1];
	Head_Data.resize(length);
	fread(Head_Data.data(), sizeof(uint8_t), length, file);
	fclose(file);
}
void read_List_Length(string filename)
{
	FILE *lengthfile = fopen((filename + ".list-l").c_str(), "rb");
	unsigned tmplength = 0;
	int64_t prelength = 0;
	while (fread(&tmplength, sizeof(unsigned), 1, lengthfile))
	{
		List_offset.push_back(prelength);
		prelength += tmplength;
	}
	List_offset.push_back(prelength);
	cout << "All we have " << List_offset.size() - 1 << " lists" << endl;
	fclose(lengthfile);
}
void read_BlockWand_Data(string filename)
{
	FILE *file = fopen((filename + ".BMWwand").c_str(), "rb");
	uint64_t length = 0;
	fread(&length, sizeof(uint64_t), 1, file); 
	fread(&length, sizeof(uint64_t), 1, file); 
	Block_Start.resize(length);
	fread(Block_Start.data(), sizeof(uint64_t), length, file);
	fread(&length, sizeof(uint64_t), 1, file); cout << "All we have " << length << " blocks" << endl;
	Block_Max_Term_Weight.resize(length);
	fread(Block_Max_Term_Weight.data(), sizeof(float), length, file);
	fread(&length, sizeof(uint64_t), 1, file); cout << "All we have " << length << " blocks" << endl;
	Block_Docid.resize(length);
	fread(Block_Docid.data(), sizeof(uint32_t), length, file);
	fclose(file);
}

void read_query(string filename)
{
	queries.clear();
	ifstream fin(filename);
	string str = "";
	while (getline(fin, str))
	{
		istringstream sin(str);
		string field = "";
		term_id_vec tmpq;
		while (getline(sin, field, '\t'))
			tmpq.push_back(atoi(field.c_str()));
		queries.push_back(tmpq);
	}
}



void remove_duplicate_terms(term_id_vec& terms)
{
	std::sort(terms.begin(), terms.end());
	terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
}

typedef quasi_succinct::bm25 scorer_type;

typedef typename block_posting_list::document_enumerator enum_type;
typedef wand_data_enumerator wdata_enum;

struct scored_enum {
	enum_type docs_enum;
	wdata_enum w;
	float q_weight;
	float max_weight;
};

inline double get_time_usecs() {
	timeval tv;
	gettimeofday(&tv, NULL);
	return double(tv.tv_sec) * 1000000 + double(tv.tv_usec);
}

void aioReadFirstBlock()
{
	int threadid = omp_get_thread_num();
	unsigned cur = 0, i = 0;
	for (i = 0, cur = 0; i < AIOreadinfo[threadid].size(); i++)
	{
		unsigned lid = i;
		io_prep_pread(&readrequest[threadid][cur], IndexFile, list_data[threadid] + AIOreadinfo[threadid][lid].memoffset, AIOreadinfo[threadid][lid].readlength, AIOreadinfo[threadid][lid].readoffset);
		listrequest[threadid][cur] = &readrequest[threadid][cur];
		AIOreadinfo[threadid][lid].memoffset += AIOreadinfo[threadid][lid].readlength;
		AIOreadinfo[threadid][lid].readoffset += AIOreadinfo[threadid][lid].readlength;
		AIOreadinfo[threadid][lid].curSendpos += AIOreadinfo[threadid][lid].readlength;
		cur++;
	}
	curAIOLIST_SIZE[threadid] = cur;
	int ret = io_submit(ctx[threadid], curAIOLIST_SIZE[threadid], listrequest[threadid].data());
	if (ret != curAIOLIST_SIZE[threadid]) {
		perror("io_submit"); 
	}
}

struct and_query {
	uint64_t operator()(term_id_vec terms)const
	{
		int threadid = omp_get_thread_num();
		if (terms.empty()) return 0;
		remove_duplicate_terms(terms);
		typedef typename block_posting_list::document_enumerator enum_type;
		std::vector<enum_type> enums;
		enums.reserve(terms.size());
		AIOreadinfo[threadid].clear();
		AIOreadinfo[threadid].reserve(terms.size());
		int64_t tmpoff = 0, tmpcount = 0; 
		for (auto term : terms)
		{
			AIOReadInfo tmpaio;
			tmpaio.listlength = List_offset[term + 1] - List_offset[term];
			tmpaio.memoffset = tmpoff;
			int64_t offset = List_offset[term];
			tmpaio.readoffset = ((int64_t)(offset / DISK_BLOCK))*DISK_BLOCK;
			tmpaio.readlength = READ_BLOCK;
			tmpaio.tid = term;
			tmpaio.curReadpos = tmpaio.readoffset - offset;;
			tmpaio.curSendpos = tmpaio.curReadpos;
			AIOreadinfo[threadid].push_back(tmpaio);
			int64_t readlength = ((int64_t)(ceil((double)(tmpaio.listlength + offset - tmpaio.readoffset) / READ_BLOCK)))*READ_BLOCK;
			enum_type tmplist(Head_Data.data() + Head_offset[term], list_data[threadid] + tmpoff + offset - tmpaio.readoffset, num_docs, term, tmpcount);
			enums.push_back(tmplist);
			tmpoff += readlength;
			tmpcount++;
		}
		aioReadFirstBlock();

		for (unsigned i = 0; i < enums.size(); i++)
		{
			enums[i].reset();
		}
		// sort by increasing frequency
		std::sort(enums.begin(), enums.end(),
			[](enum_type const& lhs, enum_type const& rhs) {
			return lhs.size() < rhs.size();
		});

		uint64_t results = 0;
		uint64_t candidate = enums[0].docid();
		size_t i = 1;
		while (candidate < num_docs) {
			for (; i < enums.size(); ++i) {
				enums[i].next_geq(candidate);
				if (enums[i].docid() != candidate) {
					candidate = enums[i].docid();
					i = 0;
					break;
				}
			}

			if (i == enums.size()) {
				results += 1;
				enums[0].next();
				candidate = enums[0].docid();
				i = 1;
			}
		}
		return results;
	}

};
typedef std::pair<uint64_t, uint64_t> term_freq_pair;
typedef std::vector<term_freq_pair> term_freq_vec;
term_freq_vec query_freqs(term_id_vec terms)
{
	term_freq_vec query_term_freqs;
	std::sort(terms.begin(), terms.end());
	for (size_t i = 0; i < terms.size(); ++i) {
		if (i == 0 || terms[i] != terms[i - 1]) {
			query_term_freqs.emplace_back(terms[i], 1);
		}
		else {
			query_term_freqs.back().second += 1;
		}
	}

	return query_term_freqs;
}
struct topk_queue {
	topk_queue(uint64_t k)
	: m_k(k)
	{}

	bool insert(float score)
	{
		if (m_q.size() < m_k) {
			m_q.push_back(score);
			std::push_heap(m_q.begin(), m_q.end(), std::greater<float>());
			return true;
		}
		else {
			if (score > m_q.front()) {
				std::pop_heap(m_q.begin(), m_q.end(), std::greater<float>());
				m_q.back() = score;
				std::push_heap(m_q.begin(), m_q.end(), std::greater<float>());
				return true;
			}
		}
		return false;
	}

	bool would_enter(float score) const
	{
		return m_q.size() < m_k || score > m_q.front();
	}

	void finalize()
	{
		std::sort_heap(m_q.begin(), m_q.end(), std::greater<float>());
	}

	std::vector<float> const& topk() const
	{
		return m_q;
	}

	void clear()
	{
		m_q.clear();
	}
	void test_write_topK(string str)
	{
		int threadid = omp_get_thread_num();
		ofstream fout(indexFileName + str + "_result" + to_string(threadid) + ".txt", ios::app);
		for (unsigned i = 0; i < topk().size(); i++)
			fout << m_q[i] << " ";
		fout << endl;
		fout.close();
	}
private:
	uint64_t m_k;
	std::vector<float> m_q;
};

struct block_max_wand_query {

	block_max_wand_query(quasi_succinct::wand_data<scorer_type> const& wdata, uint64_t k)
	: m_wdata(wdata), m_topk(k) {
	}



	uint64_t operator()(term_id_vec terms) {
		m_topk.clear();

		int threadid = omp_get_thread_num();
		if (terms.empty()) return 0;
		auto query_term_freqs = query_freqs(terms);

		std::vector<scored_enum> enums;
		enums.reserve(query_term_freqs.size());
		uint64_t tmpnum_docs = num_docs;

		AIOreadinfo[threadid].clear();
		AIOreadinfo[threadid].reserve(terms.size());
		int64_t tmpoff = 0, tmpcount = 0;
		for (auto term : query_term_freqs)
		{
			AIOReadInfo tmpaio;
			tmpaio.listlength = List_offset[term.first + 1] - List_offset[term.first];
			tmpaio.memoffset = tmpoff;
			int64_t offset = List_offset[term.first];
			tmpaio.readoffset = ((int64_t)(offset / DISK_BLOCK))*DISK_BLOCK;
			tmpaio.readlength = READ_BLOCK;
			tmpaio.tid = term.first;
			tmpaio.curReadpos = tmpaio.readoffset - offset;;
			tmpaio.curSendpos = tmpaio.curReadpos;
			AIOreadinfo[threadid].push_back(tmpaio);

			int64_t readlength = ((int64_t)(ceil((double)(tmpaio.listlength + offset - tmpaio.readoffset) / READ_BLOCK)))*READ_BLOCK;
			enum_type list(Head_Data.data() + Head_offset[term.first], list_data[threadid] + tmpoff + offset - tmpaio.readoffset, num_docs, term.first, tmpcount);
			wdata_enum w_enum(Block_Start[term.first], Block_Start[term.first + 1] - Block_Start[term.first], Block_Max_Term_Weight, Block_Docid);
			auto q_weight = scorer_type::query_term_weight
				(term.second, list.size(), tmpnum_docs);
			auto max_weight = q_weight * m_wdata.max_term_weight(term.first);
			enums.push_back(scored_enum{ std::move(list), w_enum, q_weight, max_weight });
			tmpoff += readlength;
			tmpcount++;
		}
		aioReadFirstBlock();

		for (unsigned i = 0; i < enums.size(); i++)
			enums[i].docs_enum.reset();

		std::vector<scored_enum *> ordered_enums;
		ordered_enums.reserve(enums.size());
		for (auto &en : enums) {
			ordered_enums.push_back(&en);
		}


		auto sort_enums = [&]() {
			// sort enumerators by increasing docid
			std::sort(ordered_enums.begin(), ordered_enums.end(),
				[](scored_enum *lhs, scored_enum *rhs) {
				return lhs->docs_enum.docid() < rhs->docs_enum.docid();
			});
		};


		sort_enums();

		while (true) {

			// find pivot
			float upper_bound = 0.f;
			size_t pivot;
			bool found_pivot = false;
			uint64_t pivot_id = num_docs;

			for (pivot = 0; pivot < ordered_enums.size(); ++pivot) {
				if (ordered_enums[pivot]->docs_enum.docid() == num_docs) {
					break;
				}

				upper_bound += ordered_enums[pivot]->max_weight;
				if (m_topk.would_enter(upper_bound)) {
					found_pivot = true;
					pivot_id = ordered_enums[pivot]->docs_enum.docid();
					for (; pivot + 1 < ordered_enums.size() &&
						ordered_enums[pivot + 1]->docs_enum.docid() == pivot_id; ++pivot);
						break;
				}
			}

			// no pivot found, we can stop the search
			if (!found_pivot) {
				break;
			}

			double block_upper_bound = 0;

			for (size_t i = 0; i < pivot + 1; ++i) {
				if (ordered_enums[i]->w.docid() < pivot_id) {
					ordered_enums[i]->w.next_geq(pivot_id);
				}

				block_upper_bound += ordered_enums[i]->w.score() * ordered_enums[i]->q_weight;
			}


			if (m_topk.would_enter(block_upper_bound)) {


				// check if pivot is a possible match
				if (pivot_id == ordered_enums[0]->docs_enum.docid()) {
					float score = 0;
					float norm_len = m_wdata.norm_len(pivot_id);

					for (scored_enum *en : ordered_enums) {
						if (en->docs_enum.docid() != pivot_id) {
							break;
						}
						float part_score = en->q_weight * scorer_type::doc_term_weight
							(en->docs_enum.freq(), norm_len);
						score += part_score;
						block_upper_bound -= en->w.score() * en->q_weight - part_score;
						if (!m_topk.would_enter(block_upper_bound)) {
							break;
						}

					}
					for (scored_enum *en : ordered_enums) {
						if (en->docs_enum.docid() != pivot_id) {
							break;
						}
						en->docs_enum.next();
					}

					m_topk.insert(score);
					// resort by docid
					sort_enums();

				}
				else {

					uint64_t next_list = pivot;
					for (; ordered_enums[next_list]->docs_enum.docid() == pivot_id;
						--next_list);
						ordered_enums[next_list]->docs_enum.next_geq(pivot_id);

					// bubble down the advanced list
					for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
						if (ordered_enums[i]->docs_enum.docid() <=
							ordered_enums[i - 1]->docs_enum.docid()) {
							std::swap(ordered_enums[i], ordered_enums[i - 1]);
						}
						else {
							break;
						}
					}
				}

			}
			else {


				uint64_t next;
				uint64_t next_list = pivot;

				float q_weight = ordered_enums[next_list]->q_weight;


				for (uint64_t i = 0; i < pivot; i++){
					if (ordered_enums[i]->q_weight > q_weight){
						next_list = i;
						q_weight = ordered_enums[i]->q_weight;
					}
				}

				// TO BE FIXED (change with num_docs())
				uint64_t next_jump = uint64_t(-2);

				if (pivot + 1 < ordered_enums.size()) {
					next_jump = ordered_enums[pivot + 1]->docs_enum.docid();
				}


				for (size_t i = 0; i <= pivot; ++i){
					if (ordered_enums[i]->w.docid() < next_jump)
						next_jump = std::min(ordered_enums[i]->w.docid(), next_jump);
				}

				next = next_jump + 1;
				if (pivot + 1 < ordered_enums.size()) {
					if (next > ordered_enums[pivot + 1]->docs_enum.docid()) {
						next = ordered_enums[pivot + 1]->docs_enum.docid();
					}
				}

				if (next <= ordered_enums[pivot]->docs_enum.docid()) {
					next = ordered_enums[pivot]->docs_enum.docid() + 1;
				}

				ordered_enums[next_list]->docs_enum.next_geq(next);

				// bubble down the advanced list
				for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
					if (ordered_enums[i]->docs_enum.docid() <
						ordered_enums[i - 1]->docs_enum.docid()) {
						std::swap(ordered_enums[i], ordered_enums[i - 1]);
					}
					else {
						break;
					}
				}
			}
		}

		m_topk.finalize();
		//m_topk.test_write_topK("BMW");
		return m_topk.topk().size();
	}


	std::vector<float> const& topk() const
	{
		return m_topk.topk();
	}

private:
	quasi_succinct::wand_data<scorer_type> const& m_wdata;
	topk_queue m_topk;
};


struct wand_query {

	wand_query(quasi_succinct::wand_data<scorer_type> const& wdata, uint64_t k)
	: m_wdata(wdata)
	, m_topk(k)
	{}

	uint64_t operator()(term_id_vec const& terms)
	{
		int threadid = omp_get_thread_num();
		m_topk.clear();
		if (terms.empty()) return 0;
		auto query_term_freqs = query_freqs(terms);

		uint64_t tmpnum_docs = num_docs;

		std::vector<scored_enum> enums;
		enums.reserve(query_term_freqs.size());

		AIOreadinfo[threadid].clear();
		AIOreadinfo[threadid].reserve(terms.size());
		int64_t tmpoff = 0, tmpcount = 0;
		for (auto term : query_term_freqs)
		{
			AIOReadInfo tmpaio;
			tmpaio.listlength = List_offset[term.first + 1] - List_offset[term.first];
			tmpaio.memoffset = tmpoff;
			int64_t offset = List_offset[term.first];
			tmpaio.readoffset = ((int64_t)(offset / DISK_BLOCK))*DISK_BLOCK;
			tmpaio.readlength = READ_BLOCK;
			tmpaio.tid = term.first;
			tmpaio.curReadpos = tmpaio.readoffset - offset;;
			tmpaio.curSendpos = tmpaio.curReadpos;
			AIOreadinfo[threadid].push_back(tmpaio);

			int64_t readlength = ((int64_t)(ceil((double)(tmpaio.listlength + offset - tmpaio.readoffset) / READ_BLOCK)))*READ_BLOCK;
			enum_type list(Head_Data.data() + Head_offset[term.first], list_data[threadid] + tmpoff + offset - tmpaio.readoffset, num_docs, term.first, tmpcount);
			wdata_enum w_enum(Block_Start[term.first], Block_Start[term.first + 1] - Block_Start[term.first], Block_Max_Term_Weight, Block_Docid);
			auto q_weight = scorer_type::query_term_weight
				(term.second, list.size(), tmpnum_docs);
			auto max_weight = q_weight * m_wdata.max_term_weight(term.first);
			enums.push_back(scored_enum{ std::move(list), w_enum, q_weight, max_weight });
			tmpoff += readlength;
			tmpcount++;
		}
		aioReadFirstBlock();

		for (unsigned i = 0; i < enums.size(); i++)
			enums[i].docs_enum.reset();

		std::vector<scored_enum*> ordered_enums;
		ordered_enums.reserve(enums.size());
		for (auto& en : enums) {
			ordered_enums.push_back(&en);
		}

		auto sort_enums = [&]() {
			// sort enumerators by increasing docid
			std::sort(ordered_enums.begin(), ordered_enums.end(),
				[](scored_enum* lhs, scored_enum* rhs) {
				return lhs->docs_enum.docid() < rhs->docs_enum.docid();
			});
		};
		sort_enums();
		while (true) {
			// find pivot
			float upper_bound = 0;
			size_t pivot;
			bool found_pivot = false;
			for (pivot = 0; pivot < ordered_enums.size(); ++pivot) {
				if (ordered_enums[pivot]->docs_enum.docid() == tmpnum_docs) {
					break;
				}
				upper_bound += ordered_enums[pivot]->max_weight;
				if (m_topk.would_enter(upper_bound)) {
					found_pivot = true;
					break;
				}
			}
			// no pivot found, we can stop the search
			if (!found_pivot) {
				break;
			}

			// check if pivot is a possible match
			uint64_t pivot_id = ordered_enums[pivot]->docs_enum.docid();
			if (pivot_id == ordered_enums[0]->docs_enum.docid()) {
				float score = 0;
				float norm_len = m_wdata.norm_len(pivot_id);
				for (scored_enum* en : ordered_enums) {
					if (en->docs_enum.docid() != pivot_id) {
						break;
					}
					score += en->q_weight * scorer_type::doc_term_weight
						(en->docs_enum.freq(), norm_len);
					en->docs_enum.next();
				}

				m_topk.insert(score);
				// resort by docid
				sort_enums();
			}
			else {
				// no match, move farthest list up to the pivot
				uint64_t next_list = pivot;
				for (; ordered_enums[next_list]->docs_enum.docid() == pivot_id;
					--next_list);
					ordered_enums[next_list]->docs_enum.next_geq(pivot_id);
				// bubble down the advanced list
				for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
					if (ordered_enums[i]->docs_enum.docid() <
						ordered_enums[i - 1]->docs_enum.docid()) {
						std::swap(ordered_enums[i], ordered_enums[i - 1]);
					}
					else {
						break;
					}
				}
			}
		}
		m_topk.finalize();
		//m_topk.test_write_topK("WAND");
		return m_topk.topk().size();
	}

	std::vector<float> const& topk() const
	{
		return m_topk.topk();
	}

private:
	quasi_succinct::wand_data<scorer_type> const& m_wdata;
	topk_queue m_topk;
};
struct maxscore_query {

	maxscore_query(quasi_succinct::wand_data<scorer_type> const& wdata, uint64_t k)
	: m_wdata(wdata)
	, m_topk(k)
	{}

	uint64_t operator()(term_id_vec const& terms)
	{
		int threadid = omp_get_thread_num();
		m_topk.clear();
		if (terms.empty()) return 0;
		auto query_term_freqs = query_freqs(terms);

		uint64_t tmpnum_docs = num_docs;

		std::vector<scored_enum> enums;
		enums.reserve(query_term_freqs.size());

		AIOreadinfo[threadid].clear();
		AIOreadinfo[threadid].reserve(terms.size());
		int64_t tmpoff = 0, tmpcount = 0;
		for (auto term : query_term_freqs)
		{
			AIOReadInfo tmpaio;
			tmpaio.listlength = List_offset[term.first + 1] - List_offset[term.first];
			tmpaio.memoffset = tmpoff;
			int64_t offset = List_offset[term.first];
			tmpaio.readoffset = ((int64_t)(offset / DISK_BLOCK))*DISK_BLOCK;
			tmpaio.readlength = READ_BLOCK;
			tmpaio.tid = term.first;
			tmpaio.curReadpos = tmpaio.readoffset - offset;;
			tmpaio.curSendpos = tmpaio.curReadpos;
			AIOreadinfo[threadid].push_back(tmpaio);

			int64_t readlength = ((int64_t)(ceil((double)(tmpaio.listlength + offset - tmpaio.readoffset) / READ_BLOCK)))*READ_BLOCK;
			enum_type list(Head_Data.data() + Head_offset[term.first], list_data[threadid] + tmpoff + offset - tmpaio.readoffset, num_docs, term.first, tmpcount);
			wdata_enum w_enum(Block_Start[term.first], Block_Start[term.first + 1] - Block_Start[term.first], Block_Max_Term_Weight, Block_Docid);
			auto q_weight = scorer_type::query_term_weight
				(term.second, list.size(), tmpnum_docs);
			auto max_weight = q_weight * m_wdata.max_term_weight(term.first);
			enums.push_back(scored_enum{ std::move(list), w_enum, q_weight, max_weight });
			tmpoff += readlength;
			tmpcount++;
		}
		aioReadFirstBlock();

		for (unsigned i = 0; i < enums.size(); i++)
			enums[i].docs_enum.reset();

		std::vector<scored_enum*> ordered_enums;
		ordered_enums.reserve(enums.size());
		for (auto& en : enums) {
			ordered_enums.push_back(&en);
		}

		// sort enumerators by increasing maxscore
		std::sort(ordered_enums.begin(), ordered_enums.end(),
			[](scored_enum* lhs, scored_enum* rhs) {
			return lhs->max_weight < rhs->max_weight;
		});

		std::vector<float> upper_bounds(ordered_enums.size());
		upper_bounds[0] = ordered_enums[0]->max_weight;
		for (size_t i = 1; i < ordered_enums.size(); ++i) {
			upper_bounds[i] = upper_bounds[i - 1] + ordered_enums[i]->max_weight;
		}

		uint64_t non_essential_lists = 0;
		uint64_t cur_doc =
			std::min_element(enums.begin(), enums.end(),
			[](scored_enum const& lhs, scored_enum const& rhs) {
			return lhs.docs_enum.docid() < rhs.docs_enum.docid();
		})
			->docs_enum.docid();

		while (non_essential_lists < ordered_enums.size() &&
			cur_doc < tmpnum_docs) {
			float score = 0;
			float norm_len = m_wdata.norm_len(cur_doc);
			uint64_t next_doc = tmpnum_docs;
			for (size_t i = non_essential_lists; i < ordered_enums.size(); ++i) {
				if (ordered_enums[i]->docs_enum.docid() == cur_doc) {
					score += ordered_enums[i]->q_weight * scorer_type::doc_term_weight
						(ordered_enums[i]->docs_enum.freq(), norm_len);
					ordered_enums[i]->docs_enum.next();
				}
				if (ordered_enums[i]->docs_enum.docid() < next_doc) {
					next_doc = ordered_enums[i]->docs_enum.docid();
				}
			}

			// try to complete evaluation with non-essential lists
			for (size_t i = non_essential_lists - 1; i + 1 > 0; --i) {
				if (!m_topk.would_enter(score + upper_bounds[i])) {
					break;
				}
				ordered_enums[i]->docs_enum.next_geq(cur_doc);
				if (ordered_enums[i]->docs_enum.docid() == cur_doc) {
					score += ordered_enums[i]->q_weight * scorer_type::doc_term_weight
						(ordered_enums[i]->docs_enum.freq(), norm_len);
				}
			}

			if (m_topk.insert(score)) {
				// update non-essential lists
				while (non_essential_lists < ordered_enums.size() &&
					!m_topk.would_enter(upper_bounds[non_essential_lists])) {
					non_essential_lists += 1;
				}
			}

			cur_doc = next_doc;
		}

		m_topk.finalize();
		//m_topk.test_write_topK("MAXSCORE");
		return m_topk.topk().size();
	}

	std::vector<float> const& topk() const
	{
		return m_topk.topk();
	}

private:
	quasi_succinct::wand_data<scorer_type> const& m_wdata;
	topk_queue m_topk;
};
struct ranked_and_query {

	ranked_and_query(quasi_succinct::wand_data<scorer_type> const& wdata, uint64_t k)
	: m_wdata(wdata)
	, m_topk(k)
	{}

	uint64_t operator()(term_id_vec terms)
	{
		int threadid = omp_get_thread_num();
		m_topk.clear();
		if (terms.empty()) return 0;
		auto query_term_freqs = query_freqs(terms);

		uint64_t tmpnum_docs = num_docs;
		std::vector<scored_enum> enums;
		enums.reserve(query_term_freqs.size());

		AIOreadinfo[threadid].clear();
		AIOreadinfo[threadid].reserve(terms.size());
		int64_t tmpoff = 0, tmpcount = 0;
		for (auto term : query_term_freqs)
		{
			AIOReadInfo tmpaio;
			tmpaio.listlength = List_offset[term.first + 1] - List_offset[term.first];
			tmpaio.memoffset = tmpoff;
			int64_t offset = List_offset[term.first];
			tmpaio.readoffset = ((int64_t)(offset / DISK_BLOCK))*DISK_BLOCK;
			tmpaio.readlength = READ_BLOCK;
			tmpaio.tid = term.first;
			tmpaio.curReadpos = tmpaio.readoffset - offset;;
			tmpaio.curSendpos = tmpaio.curReadpos;
			AIOreadinfo[threadid].push_back(tmpaio);

			int64_t readlength = ((int64_t)(ceil((double)(tmpaio.listlength + offset - tmpaio.readoffset) / READ_BLOCK)))*READ_BLOCK;
			enum_type list(Head_Data.data() + Head_offset[term.first], list_data[threadid] + tmpoff + offset - tmpaio.readoffset, num_docs, term.first, tmpcount);
			wdata_enum w_enum(Block_Start[term.first], Block_Start[term.first + 1] - Block_Start[term.first], Block_Max_Term_Weight, Block_Docid);
			auto q_weight = scorer_type::query_term_weight
				(term.second, list.size(), tmpnum_docs);
			auto max_weight = q_weight * m_wdata.max_term_weight(term.first);
			enums.push_back(scored_enum{ std::move(list), w_enum, q_weight, max_weight });
			tmpoff += readlength;
			tmpcount++;
		}
		aioReadFirstBlock();

		for (unsigned i = 0; i < enums.size(); i++)
			enums[i].docs_enum.reset();

		std::vector<scored_enum*> ordered_enums;
		ordered_enums.reserve(enums.size());
		for (auto& en : enums) {
			ordered_enums.push_back(&en);
		}

		// sort enumerators by increasing freq
		std::sort(ordered_enums.begin(), ordered_enums.end(),
			[](scored_enum* lhs, scored_enum* rhs) {
			return lhs->docs_enum.size() < rhs->docs_enum.size();
		});

		uint64_t candidate = ordered_enums[0]->docs_enum.docid();
		size_t i = 1;
		while (candidate < tmpnum_docs) {
			for (; i < ordered_enums.size(); ++i) {
				ordered_enums[i]->docs_enum.next_geq(candidate);
				if (ordered_enums[i]->docs_enum.docid() != candidate) {
					candidate = ordered_enums[i]->docs_enum.docid();
					i = 0;
					break;
				}
			}

			if (i == enums.size()) {
				float norm_len = m_wdata.norm_len(candidate);
				float score = 0;
				for (i = 0; i < enums.size(); ++i) {
					score += ordered_enums[i]->q_weight * scorer_type::doc_term_weight
						(ordered_enums[i]->docs_enum.freq(), norm_len);
				}

				m_topk.insert(score);
				ordered_enums[0]->docs_enum.next();
				candidate = ordered_enums[0]->docs_enum.docid();
				i = 1;
			}
		}

		m_topk.finalize();
		//m_topk.test_write_topK("RANKAND");
		return m_topk.topk().size();
	}

	std::vector<float> const& topk() const
	{
		return m_topk.topk();
	}

private:
	quasi_succinct::wand_data<scorer_type> const& m_wdata;
	topk_queue m_topk;
};


void initial_data()
{
	string filename = indexFileName;
	read_Head_Length(filename);
	read_Head_Data(filename);
	read_List_Length(filename);
	read_BlockWand_Data(filename);
	read_query(filename + "Query.txt");
	initThreadStruct();
}
void print_statistics(vector<vector<double>>times, string querytype)
{
	vector<double>query_times;
	for (int i = 0; i < threadCount; i++)
		query_times.insert(query_times.end(), times[i].begin(), times[i].end());

	cout << "query times count=" << query_times.size() << endl;
	std::sort(query_times.begin(), query_times.end());
	double avg = std::accumulate(query_times.begin(), query_times.end(), double()) / query_times.size();
	double q25 = query_times[25 * query_times.size() / 100];
	double q50 = query_times[query_times.size() / 2];
	double q75 = query_times[75 * query_times.size() / 100];
	double q95 = query_times[95 * query_times.size() / 100];
	double q99 = query_times[99 * query_times.size() / 100];
	cout << "----------" + querytype + "----------" << endl;
	cout << "Mean: " << avg << std::endl;
	cout << "25% quantile: " << q25 << std::endl;
	cout << "50% quantile: " << q50 << std::endl;
	cout << "75% quantile: " << q75 << std::endl;
	cout << "95% quantile: " << q95 << std::endl;
	cout << "99% quantile: " << q99 << std::endl;
}


template <class T>
inline void do_not_optimize_away(T&& datum) {
	asm volatile("" : "+r" (datum));
}

void receiveLastRequest()
{
	int threadid = omp_get_thread_num();
	while (io_getevents(ctx[threadid], curAIOLIST_SIZE[threadid], MAXREQUEST, events[threadid].data(), NULL) != curAIOLIST_SIZE[threadid])
	{
		;
	}
}
template <typename QueryOperator>
void perform_query(QueryOperator&& query_op, string querytype, vector<unsigned>query)
{
	int threadid = omp_get_thread_num();
	curReadID[threadid] = 0;
	auto tick = get_time_usecs();
	uint64_t result = query_op(query);
	do_not_optimize_away(result);
	double elapsed = double(get_time_usecs() - tick);
	receiveLastRequest();
	query_Times[threadid].push_back(elapsed);
}

int main(int argc, char *argv[])
{
	indexFileName = argv[1];
	string querytype = argv[2];
	threadCount = atoi(argv[3]);
	IndexFile = open((indexFileName + ".list").c_str(), O_RDONLY | O_DIRECT);
	initial_data();
	cout << "initial data over" << endl;

	if (querytype == "AND")
	{
		auto tick = get_time_usecs();
#pragma omp parallel for num_threads(threadCount) schedule(dynamic)
		for (unsigned i = 0; i < queries.size(); i++)
			perform_query(and_query(), "AND", queries[i]);
		double elapsed = double(get_time_usecs() - tick);
		cout << "Performed AND query, sum time=" << elapsed << "throught put=" << queries.size() / elapsed * 1000000 << endl;
	}
	else
	{
		quasi_succinct::wand_data<> wdata;
		boost::iostreams::mapped_file_source md(indexFileName + ".wand");
		succinct::mapper::map(wdata, md, succinct::mapper::map_flags::warmup);
		if (querytype == "MAXSCORE")
		{
			auto tick = get_time_usecs();
#pragma omp parallel for num_threads(threadCount) schedule(dynamic)
			for (unsigned i = 0; i < queries.size(); i++)
				perform_query(maxscore_query(wdata, 10), "MAXSCORE", queries[i]);
			double elapsed = double(get_time_usecs() - tick);
			cout << "Performed MAXSCORE query, sum time=" << elapsed << "throught put=" << queries.size() / elapsed * 1000000 << endl;
		}
		else if (querytype == "WAND")
		{
			auto tick = get_time_usecs();
#pragma omp parallel for num_threads(threadCount) schedule(dynamic)
			for (unsigned i = 0; i < queries.size(); i++)
				perform_query(wand_query(wdata, 10), "WAND", queries[i]);
			double elapsed = double(get_time_usecs() - tick);
			cout << "Performed WAND query, sum time=" << elapsed << "throught put=" << queries.size() / elapsed * 1000000 << endl;
		}
		else if (querytype == "RANKAND")
		{
			auto tick = get_time_usecs();
#pragma omp parallel for num_threads(threadCount) schedule(dynamic)
			for (unsigned i = 0; i < queries.size(); i++)
				perform_query(ranked_and_query(wdata, 10), "RANKAND", queries[i]);
			double elapsed = double(get_time_usecs() - tick);
			cout << "Performed RANKAND query, sum time=" << elapsed << "throught put=" << queries.size() / elapsed * 1000000 << endl;
		}
		else if (querytype == "BMW")
		{
			auto tick = get_time_usecs();
#pragma omp parallel for num_threads(threadCount)schedule(dynamic)
			for (unsigned i = 0; i < queries.size(); i++)
				perform_query(block_max_wand_query(wdata, 10), "BMW", queries[i]);
			double elapsed = double(get_time_usecs() - tick);
			cout << "Performed BMW query, sum time=" << elapsed << "throught put=" << queries.size() / elapsed * 1000000 << endl;
		}
	}
	close(IndexFile);

	print_statistics(query_Times, querytype);
	for (int i = 0; i < threadCount; i++)
		io_destroy(ctx[i]);
}
