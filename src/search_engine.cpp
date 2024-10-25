#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <queue>
#include <cmath>
#include <sstream>
#include <cstdint>
#include <zlib.h>

const int POSTING_PER_BLOCK = 128;
const std::string LEXICON_FILE = "final_sorted_lexicon.txt";
const std::string INDEX_FILE = "final_sorted_index.bin";
const std::string DOC_INFO_FILE = "document_info.txt";
const std::string BLOCK_INFO_FILE = "final_sorted_block_info2.txt";
const std::string ORIGINAL_TAR_GZ = "../src/collection.tar.gz";

// parameters
const double k1 = 1.2;
const double b = 0.75;

// decode function
uint32_t varbyteDecode(const std::vector<uint8_t> &bytes);
int varbyteDecode(const uint8_t *data, size_t &bytes_read);

struct LexiconEntry
{
    int term_id;
    int postings_num;
    int64_t start_position;
    int64_t bytes_size;
};

struct SearchResult
{
    int doc_id;
    double score;
};

class InvertedList
{
private:
    std::ifstream &index_file_;
    int64_t start_pos_;
    int64_t bytes_size_;
    int64_t block_doc_id_reader;
    int64_t block_doc_id_size;
    int64_t block_freq_reader;
    int64_t block_freq_size;
    std::vector<std::pair<int, std::pair<int64_t, std::pair<int64_t, int64_t>>>> &block_info_;
    std::vector<uint8_t> current_block_;
    int current_block_index_;

    void loadBlockIndex()
    {
        std::cout << "Loading block index." << std::endl;
        if (current_block_index_ == -1)
        {

            for (int i = 0; i < block_info_.size(); ++i)
            {
                if (start_pos_ < block_info_[i].second.first)
                {
                    current_block_index_ = i - 1;
                    break;
                }
            }
        }
        else
        {
            current_block_index_++;
        }
        std::cout << "Block index loaded. Current block index: " << current_block_index_ << std::endl;
    }

    bool openBlock()
    {
        std::cout << "Opening block." << std::endl;
        block_doc_id_reader = block_info_[current_block_index_].second.first;
        block_doc_id_size = block_info_[current_block_index_].second.second.first;
        block_freq_reader = block_doc_id_reader + block_doc_id_size;
        block_freq_size = block_info_[current_block_index_].second.second.second;
        index_file_.seekg(block_doc_id_reader);
        int64_t block_size = block_doc_id_size + block_freq_size;
        bytes_size_ -= block_size;
        current_block_.resize(block_size);
        index_file_.read(reinterpret_cast<char *>(current_block_.data()), block_size); // read the block into memory
        return bytes_size_ + block_size >= 0;
    }

    bool loadNextBlock()
    {
        std::cout << "Loading next block." << std::endl;
        loadBlockIndex();

        if (current_block_index_ == block_info_.size()) // no more blocks
        {
            return false;
        }

        return openBlock();
    }

public:
    InvertedList(std::ifstream &index_file, int64_t start_pos, int64_t bytes_size, std::vector<std::pair<int, std::pair<int64_t, std::pair<int64_t, int64_t>>>> &block_info)
        : index_file_(index_file), start_pos_(start_pos), bytes_size_(bytes_size), block_info_(block_info)
    {
        std::cout << "Inverted list initialized. Start pos: " << start_pos_ << ", Size: " << bytes_size_ << " bytes." << std::endl;
        block_doc_id_reader = -1;
        current_block_index_ = -1;
    }

    bool next(int &doc_id, int &freq)
    {
        if (current_block_index_ == -1 || block_freq_reader >= block_info_[current_block_index_].second.first + block_doc_id_size + block_freq_size)
        {
            if (!loadNextBlock())
            {
                std::cout << "No more blocks." << std::endl;
                return false;
            }
        }

        size_t bytes_read = 0;
        std::cout << "Start to find next" << std::endl;
        // Count the number of doc_ids we need to skip, moving both doc_id and freq readers
        std::cout << "Block doc id reader: " << block_doc_id_reader << std::endl;
        std::cout << "Start pos: " << start_pos_ << std::endl;

        // check if the doc_id reader is out of range
        size_t relative_pos = block_doc_id_reader - block_info_[current_block_index_].second.first;
        std::cout << "Relative pos: " << relative_pos << std::endl;
        std::cout << "Current block size: " << current_block_.size() << std::endl;

        if (relative_pos >= current_block_.size())
        {
            std::cerr << "Relative pos is out of range" << std::endl;
            return loadNextBlock();
        }

        while (block_doc_id_reader < start_pos_ && block_doc_id_reader < block_doc_id_size + block_freq_size + block_info_[current_block_index_].second.first)
        {
            bytes_read = 0;

            // Calculate the relative position of block_doc_id_reader within the current block
            const uint8_t *doc_id_ptr = current_block_.data() + relative_pos;

            if (relative_pos >= block_doc_id_size + block_freq_size)
            {
                std::cerr << "Relative pos is out of range" << std::endl;
                return loadNextBlock();
            }

            while ((doc_id_ptr[bytes_read] & 0x80)) // MSB is 1, meaning this is not the last byte
            {
                bytes_read++;
            }
            bytes_read++;

            block_doc_id_reader += bytes_read;

            // move the frequency reader by the corresponding number of bytes
            bytes_read = 0;

            const uint8_t *freq_ptr = current_block_.data() + (block_freq_reader - block_info_[current_block_index_].second.first);

            // read until we find the last byte of the current frequency
            while ((freq_ptr[bytes_read] & 0x80)) // MSB is 1, meaning this is not the last byte
            {
                bytes_read++;
            }
            bytes_read++;

            block_freq_reader += bytes_read;
        }

        // Decode the next doc_id difference
        const uint8_t *doc_id_ptr = current_block_.data() + (block_doc_id_reader - block_info_[current_block_index_].second.first); // Pointer within the current block
        int32_t doc_id_diff = varbyteDecode(doc_id_ptr, bytes_read);                                                                // Decode using the block-relative pointer
        block_doc_id_reader += bytes_read;                                                                                          // Update the reader position by bytes_read
        std::cout << "Doc ID diff: " << doc_id_diff << std::endl;

        if (block_freq_reader + bytes_read >= block_info_[current_block_index_].second.first + block_doc_id_size + block_freq_size)
        {
            std::cerr << "Block freq reader is out of range" << std::endl;
            return loadNextBlock();
        }

        // Decode the corresponding frequency
        bytes_read = 0;                                                                                                         // Reset bytes_read for frequency decoding
        const uint8_t *freq_ptr = current_block_.data() + (block_freq_reader - block_info_[current_block_index_].second.first); // Pointer within the current block
        freq = varbyteDecode(freq_ptr, bytes_read);                                                                             // Decode using the block-relative pointer
        block_freq_reader += bytes_read;                                                                                        // Update the reader position by bytes_read
        std::cout << "Frequency: " << freq << std::endl;

        // Update the doc_id with the decoded difference
        doc_id += doc_id_diff;
        std::cout << "doc_id: " << doc_id << std::endl;

        return true;
    }
};

class SearchEngine
{
private: // private members
    std::unordered_map<std::string, LexiconEntry> lexicon;
    std::vector<std::pair<int, std::pair<int64_t, std::pair<int64_t, int64_t>>>> block;
    std::unordered_map<int, std::string> term_id_to_word;
    std::ifstream index_file;
    std::ifstream doc_info_file;
    std::ifstream original_file;
    std::vector<int64_t> lines_pos;
    std::vector<int> doc_lengths;
    int total_docs;
    double avg_doc_length;

public: // public members
    SearchEngine(const std::string &lexicon_file, const std::string &index_file,
                 const std::string &doc_info_file, const std::string &block_info_file, const std::string &original_tar_gz)
        : index_file(index_file, std::ios::binary), original_file(original_tar_gz, std::ios::binary)
    {
        loadLexicon(lexicon_file);
        loadBlockInfo(block_info_file);
        loadDocInfo(doc_info_file);
    }

    void loadLexicon(const std::string &lexicon_file)
    {
        std::ifstream lex_file(lexicon_file);
        std::unordered_map<int, std::string> term_id_to_word;
        std::string term;
        LexiconEntry entry;
        std::cout << "Loading lexicon..." << std::endl;
        while (lex_file >> term >> entry.term_id >> entry.postings_num >> entry.start_position >> entry.bytes_size) // tested
        {
            lexicon[term] = entry;
            term_id_to_word[entry.term_id] = term;
        }
        std::cout << "Lexicon loaded." << std::endl;
    }

    void loadBlockInfo(const std::string &block_info_file)
    {
        std::cout << "Loading block info..." << std::endl;
        std::ifstream block_info(block_info_file);
        int last_doc_id = 0;
        int64_t block_start_pos = 0;
        int64_t block_doc_id_size = 0;
        int64_t block_freq_size = 0;
        while (block_info >> last_doc_id >> block_doc_id_size >> block_freq_size) // tested
        {
            // std::cout << "Block info: " << last_doc_id << " " << block_doc_id_size << " " << block_freq_size << std::endl;
            block.push_back({last_doc_id, {block_start_pos, {block_doc_id_size, block_freq_size}}});
            block_start_pos += block_doc_id_size + block_freq_size;
        }
        std::cout << "Block info loaded." << std::endl;
    }

    void loadDocInfo(const std::string &doc_info_file)
    {
        std::cout << "Loading doc info..." << std::endl;
        std::ifstream doc_info(doc_info_file);
        int total_length = 0;
        int total_docs = 0;
        int doc_length;
        int64_t line_pos;
        while (doc_info >> doc_length >> line_pos) // tested
        {
            doc_lengths.push_back(doc_length);
            total_length += doc_length;
            ++total_docs;
            lines_pos.push_back(line_pos);
        }
        avg_doc_length = static_cast<double>(total_length) / total_docs;
        std::cout << "Doc info loaded." << std::endl;
    }

    std::vector<SearchResult> search(const std::string &query, bool conjunctive)
    {
        // process the query
        std::cout << "Processing query..." << std::endl;
        std::vector<std::string> terms = processQuery(query);
        std::cout << "Query processed." << std::endl;
        std::vector<InvertedList> lists;
        // find the inverted lists for the terms
        for (const auto &term : terms)
        {
            std::cout << "Searching for term: " << term << std::endl;
            if (lexicon.find(term) != lexicon.end())
            {
                std::cout << "Found term: " << term << std::endl;
                const auto &entry = lexicon[term];
                lists.emplace_back(index_file, entry.start_position, entry.bytes_size, block);
                std::cout << "Inverted list found for term: " << term << std::endl;
                std::cout << "The term starts at: " << entry.start_position << " with size: " << entry.bytes_size << std::endl;
            }
            else
            {
                std::cout << "Term not found: " << term << std::endl;
            }
        }

        // if no lists are found, return empty vector
        if (lists.empty())
            return {};

        std::vector<SearchResult> results;
        if (conjunctive)
        {
            results = conjunctiveSearch(lists);
        }
        else
        {
            results = disjunctiveSearch(lists);
        }

        std::sort(results.begin(), results.end(),
                  [](const SearchResult &a, const SearchResult &b)
                  { return a.score > b.score; });
        if (results.size() > 10)
            results.resize(10);
        std::vector<int> result_doc_ids;
        for (const auto &result : results)
        {
            result_doc_ids.push_back(result.doc_id);
        }

        return results;
    }

private: // private methods
    std::vector<std::string> processQuery(const std::string &query)
    {
        std::vector<std::string> terms;
        std::istringstream iss(query);
        std::string term;
        while (iss >> term)
        {
            std::string current_word;
            for (auto &c : term)
            {
                if (std::isalpha(c))
                {
                    current_word += std::tolower(c);
                }
                else if (std::isdigit(c))
                {
                    current_word += c;
                }
                else if (!current_word.empty())
                {
                    terms.push_back(current_word);
                    current_word.clear();
                }
            }
            if (!current_word.empty())
            {
                terms.push_back(current_word);
            }
        }
        std::cout << "Query terms: ";
        for (const auto &term : terms)
        {
            std::cout << term << " ";
        }
        std::cout << std::endl;
        return terms;
    }

    double computeIDF(int term_freq)
    {
        return std::log((total_docs - term_freq + 0.5) / (term_freq + 0.5) + 1.0);
    }

    double computeTF(int freq, int doc_length)
    {
        return (freq * (k1 + 1)) / (freq + k1 * (1 - b + b * (doc_length / avg_doc_length)));
    }

    std::vector<SearchResult> conjunctiveSearch(std::vector<InvertedList> &lists)
    {
        std::cout << "Conjunctive search..." << std::endl;
        std::vector<SearchResult> results;
        int current_doc = 0;
        std::vector<int> doc_ids(lists.size(), 0);
        std::vector<int> freqs(lists.size(), 0);

        while (true)
        {
            bool all_same_value = true;
            int max_doc_id = -1;                   // Start with an invalid doc_id value
            bool at_least_one_list_active = false; // To track if any list is still active

            // Process each inverted list
            for (size_t i = 0; i < lists.size(); ++i)
            {
                // Advance the list until we find a doc_id >= current_doc
                while (doc_ids[i] < current_doc && lists[i].next(doc_ids[i], freqs[i]))
                    ; // update doc_ids until it >= current_doc

                std::cout << "Doc ID: " << doc_ids[i] << ", Freq: " << freqs[i] << std::endl;

                if (doc_ids[i] != current_doc)
                    all_same_value = false;

                // Only consider active lists
                if (lists[i].next(doc_ids[i], freqs[i]))
                {
                    at_least_one_list_active = true;

                    // Find the maximum doc_id across all lists
                    if (doc_ids[i] > max_doc_id)
                        max_doc_id = doc_ids[i];
                }
            }

            // If no list is active anymore, stop the search
            if (!at_least_one_list_active)
                break;

            // If all lists have the same doc_id, calculate the score and move to the next document
            if (all_same_value)
            {
                if (current_doc < doc_lengths.size()) // Ensure doc_lengths[current_doc] is valid
                {
                    std::cout << "All lists have the same doc_id: " << current_doc << std::endl;
                    double score = 0;
                    int doc_length = doc_lengths[current_doc];
                    for (size_t i = 0; i < lists.size(); ++i)
                    {
                        double idf = computeIDF(lexicon[term_id_to_word[i]].postings_num);
                        double tf = computeTF(freqs[i], doc_length);
                        std::cout << "IDF: " << idf << ", TF: " << tf << std::endl;

                        score += idf * tf;
                    }
                    results.push_back({current_doc, score});
                }
                ++current_doc; // Increment to the next doc
            }
            else
            {
                // Move to the largest doc_id found across the lists to continue
                current_doc = max_doc_id;
            }

            // Exit condition: stop when the current doc_id exceeds total_docs
            if (current_doc >= total_docs || max_doc_id == -1)
                break;
        }

        return results;
    }

    std::vector<SearchResult> disjunctiveSearch(std::vector<InvertedList> &lists)
    {
        std::cout << "Disjunctive search..." << std::endl;
        std::vector<SearchResult> results;
        std::vector<int> doc_ids(lists.size(), 0);
        std::vector<int> freqs(lists.size(), 0);
        std::priority_queue<std::pair<int, int>> pq;
        std::cout << "Disjunctive search initialized." << std::endl;
        for (size_t i = 0; i < lists.size(); ++i)
        {
            if (lists[i].next(doc_ids[i], freqs[i]))
            {
                pq.push({-doc_ids[i], i});
            }
        }

        while (!pq.empty())
        {
            int cur_doc_id = -pq.top().first;
            int list_index = pq.top().second;
            pq.pop();

            double score = 0;

            // Check if cur_doc_id is within the range of doc_lengths
            if (cur_doc_id < 0 || cur_doc_id >= doc_lengths.size())
            {
                std::cerr << "Invalid cur_doc_id: " << cur_doc_id << std::endl;
                continue;
            }

            int doc_length = doc_lengths[cur_doc_id];

            for (size_t i = 0; i < lists.size(); ++i)
            {
                if (doc_ids[i] == cur_doc_id) // if the list is at the cur_doc_id
                {
                    double idf = computeIDF(lexicon[term_id_to_word[i]].postings_num);
                    double tf = computeTF(freqs[i], doc_length);
                    std::cout << "IDF: " << idf << ", TF: " << tf << std::endl;
                    score += idf * tf;

                    if (lists[i].next(doc_ids[i], freqs[i])) // if the list is not exhausted
                    {
                        pq.push({-doc_ids[i], i});
                    }
                }
            }

            results.push_back({cur_doc_id, score});
        }

        return results;
    }
};

// Varbyte decode function
uint32_t varbyteDecode(const std::vector<uint8_t> &bytes)
{
    uint32_t number = 0;
    for (int i = bytes.size() - 1; i >= 0; --i)
    {
        number = (number << 7) | (bytes[i] & 127);
    }
    return number;
}

int32_t varbyteDecode(const uint8_t *data, size_t &bytes_read)
{
    if (data == nullptr)
    {
        std::cerr << "Data is nullptr" << std::endl;
        return 0;
    }

    int32_t result = 0;
    bytes_read = 0; // Reset the byte count for this decoding
    int shift = 0;  // Shift to combine 7-bit parts of each byte

    while (true)
    {
        uint8_t byte = data[bytes_read];
        result |= ((byte & 0x7F) << shift); // Extract the lower 7 bits and shift into place
        shift += 7;                         // Prepare to add the next 7 bits

        bytes_read++; // Move to the next byte

        // If the MSB is 0, it's the last byte of this number, break out of the loop
        if ((byte & 0x80) == 0)
        {
            break;
        }
    }

    return result;
}

int main()
{
    SearchEngine engine(LEXICON_FILE,
                        INDEX_FILE,
                        DOC_INFO_FILE,
                        BLOCK_INFO_FILE,
                        ORIGINAL_TAR_GZ);

    std::string query;
    bool conjunctive;
    while (true)
    {
        std::cout << "Enter your search query (or 'q' to exit): ";
        std::getline(std::cin, query);
        if (query == "q")
            break;

        std::cout << "Enter search mode (0 for disjunctive, 1 for conjunctive): ";
        std::cin >> conjunctive;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        auto results = engine.search(query, conjunctive);

        std::cout << "Top 10 results:" << std::endl;
        for (const auto &result : results)
        {
            std::cout << "Doc ID: " << result.doc_id << ", Score: " << result.score << std::endl;
            // find line position of the original file and print the content
            // std::string content = engine.getOriginalFileContent(result.doc_id);
            // std::cout << content << std::endl;
        }
    }

    return 0;
}
