#define LOG_TAG "LatinIME: jni: LanguageModel"

#include "org_futo_inputmethod_latin_xlm_LanguageModel.h"

#include <cstring> // for memset()
#include <vector>

#include "jni.h"
#include "jni_common.h"
#include "ggml/LanguageModel.h"
#include "defines.h"
#include "suggest/core/layout/proximity_info.h"

#define EPS 0.0001
#define TIME_START(name)  const int64_t start_##name = ggml_time_us();
#define TIME_END(name)    const int64_t end_##name = ggml_time_us(); \
                          const int64_t time_taken_##name = (end_##name - start_##name) / 1000L; \
                          AKLOGI("%s:     Time taken by %s: %d ms\n", __func__, #name, (int)time_taken_##name);

static std::string trim(const std::string &s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start)) {
        start++;
    }

    auto end = s.end();
    do {
        end--;
    } while (std::distance(start, end) > 0 && std::isspace(*end));

    return {start, end + 1};
}

template<typename T>
bool sortProbabilityPairDescending(const std::pair<float, T>& a, const std::pair<float, T>& b) {
    return a.first > b.first;
}

template<typename T>
static inline void sortProbabilityPairVectorDescending(std::vector<std::pair<float, T>> &vec) {
    std::sort(vec.begin(), vec.end(), sortProbabilityPairDescending<T>);
}

template<typename T>
static inline void sortProbabilityPairVectorDescending(std::vector<std::pair<float, T>> &vec, int partial) {
    std::partial_sort(vec.begin(), vec.begin() + partial, vec.end(), sortProbabilityPairDescending<T>);
}

typedef struct potential_sequence_data {
    token_sequence tokens;
    llama_seq_id seq_id;
} potential_sequence_data;

// P = P(tokens[0]) * P(tokens[1]) * [...]
typedef std::pair<float, potential_sequence_data> potential_sequence;

static void softmax(float * input, size_t input_len) {
    float m = -INFINITY;
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] > m) {
            m = input[i];
        }
    }

    float sum = 0.0;
    for (size_t i = 0; i < input_len; i++) {
        sum += expf(input[i] - m);
    }

    float offset = m + logf(sum);
    for (size_t i = 0; i < input_len; i++) {
        input[i] = expf(input[i] - offset);
    }
}

#define NUM_TOKEN_MIX 4
struct TokenMix {
    float x;
    float y;
    struct {
        float weight;
        llama_token token;
    } mixes[NUM_TOKEN_MIX];
};


struct DecodeResult {
    int logits_head;
    int size;
};

struct LanguageModelState {
    LanguageModel *model;

    struct {
        int SPACE;

        std::vector<int> SAMPLING_BAD_TOKENS;

        int XBU;
        int XBC;
        int XEC;

        int XC0_SWIPE_MODE;

        int LETTERS_TO_IDS[26];
    } specialTokens;

    bool Initialize(const std::string &paths){
        model = LlamaAdapter::createLanguageModel(paths);
        if(!model) {
            AKLOGE("GGMLDict: Could not load model");
            return false;
        }

        specialTokens.SPACE = 560; //model->tokenToId("▁");

        specialTokens.SAMPLING_BAD_TOKENS = {
                // TODO: Don't hardcode these
                // BOS, EOS, etc and some whitespace (linebreak, tab, carriage return)
                0, 1, 2, 3, 126, 127, 128, 129, 130
        };

        for(int i = model->tokenToId(".▁"); i < model->tokenToId("0"); i++) {
            // Specifically allow the standalone dot for acronyms such as "U.S."
            // otherwise this turns into a space and we get just a nonsensical standalone "U" or similar
            // TODO: Since ". " is still blocked, we get "U.S" instead of the expected "U.S. "
            if(i == model->tokenToId(".")) continue;

            specialTokens.SAMPLING_BAD_TOKENS.emplace_back(i);
        }
        for(int i = model->tokenToId(":"); i <= model->tokenToId("~"); i++) {
            specialTokens.SAMPLING_BAD_TOKENS.emplace_back(i);
        }

        specialTokens.XBU = model->tokenToId("<XBU>");
        specialTokens.XBC = model->tokenToId("<XBC>");
        specialTokens.XEC = model->tokenToId("<XEC>");
        specialTokens.XC0_SWIPE_MODE = model->tokenToId("<XC0>");
        specialTokens.LETTERS_TO_IDS[0] = model->tokenToId("<CHAR_A>");

        ASSERT(specialTokens.XBU != 0);
        ASSERT(specialTokens.XBC != 0);
        ASSERT(specialTokens.XEC != 0);
        ASSERT(specialTokens.LETTERS_TO_IDS[0] != 0);

        for(int i = 1; i < 26; i++) {
            specialTokens.LETTERS_TO_IDS[i] = specialTokens.LETTERS_TO_IDS[0] + i;
        }

        return true;
    }

    void transform_logits(float *logits, size_t n_vocab, bool allow_space, bool allow_correction_token){
        softmax(logits, n_vocab);

        logits[specialTokens.XBU] = -999.0f;
        logits[specialTokens.XBC] = -999.0f;
        if(!allow_correction_token)
            logits[specialTokens.XEC] = -999.0f;
            
        for(int x : specialTokens.LETTERS_TO_IDS) {
            logits[x] = -999.0f;
        }

        for(int x : specialTokens.SAMPLING_BAD_TOKENS) {
            logits[specialTokens.SPACE] += std::max(0.0f, logits[x]);
            logits[x] = -999.0f;
        }

        if(!allow_space) {
            logits[specialTokens.SPACE] = -999.0f;
        }
    }

    std::vector<TokenMix> past_mixes = { };
    int GetCachedMixAmount(const std::vector<TokenMix> &mixes) {
        TIME_START(GetcachedMixAmount)
        int i = 0;
        for(i = 0; i < std::min(past_mixes.size(), mixes.size()); i++) {
            if(std::abs(past_mixes[i].x - mixes[i].x) >= EPS) break;
            if(std::abs(past_mixes[i].y - mixes[i].y) >= EPS) break;
        }

        TIME_END(GetcachedMixAmount)

        return i;
    }

    DecodeResult DecodePromptAndMixes(const token_sequence &prompt, const std::vector<TokenMix> &mixes) {
        TIME_START(PromptDecode)
        llama_context *ctx = ((LlamaAdapter *) model->adapter)->context;
        llama_batch batch = ((LlamaAdapter *) model->adapter)->batch;
        LlamaAdapter *llamaAdapter = ((LlamaAdapter *)model->adapter);

        size_t n_embd = llama_n_embd(llama_get_model(ctx));
        size_t n_vocab = llama_n_vocab(llama_get_model(ctx));

        auto prompt_ff = transformer_context_fastforward(model->transformerContext, prompt, !mixes.empty());

        //AKLOGI("prompt_ff size = %d, n_past = %d", prompt_ff.first.size(), prompt_ff.second);

        batch.n_tokens = prompt_ff.first.size();
        if(batch.n_tokens > 0) {
            for (int i = 0; i < prompt_ff.first.size(); i++) {
                batch.token[i] = prompt_ff.first[i];
                batch.pos[i] = prompt_ff.second + i;
                batch.seq_id[i][0] = 0;
                batch.n_seq_id[i] = 1;
                batch.logits[i] = false;
            }

            batch.logits[prompt_ff.first.size() - 1] = mixes.empty();


            llama_kv_cache_seq_rm(ctx, 0, prompt_ff.second, -1);

            if (llama_decode(ctx, batch) != 0) {
                AKLOGE("llama_decode() failed");
                return {};
            }
        } else {
            AKLOGI("No need to recompute prompt, proceeding to mixes");
        }

        transformer_context_apply(model->transformerContext, prompt_ff);
        TIME_END(PromptDecode)

        TIME_START(EmbedMixing)
        int size = prompt.size();
        int head = prompt_ff.first.size() - 1;

        std::vector<float> embeds;

        bool useEncoder = !llamaAdapter->encoder_weight.empty();
        AKLOGI("DecodePromptAndMixes: useEncoder=%d", useEncoder);

        for(auto &mix : mixes) {

            int num_added = 0;

            std::vector<float> mix_f(n_embd, 0.0f);

            if(useEncoder) {
                num_added = 1;

                for(size_t i=0; i<n_embd; i++) {
                    mix_f[i] = llamaAdapter->encoder_bias[i]
                            + llamaAdapter->encoder_weight[i*2]*mix.x
                            + llamaAdapter->encoder_weight[i*2 + 1]*mix.y;
                }

                //AKLOGI("DEBUG: pos %.4f %.4f got this: [%.4f %.4f %.4f %.4f %.4f %.4f %.4f ...",
                //       mix.x, mix.y,
                //             mix_f[0], mix_f[1], mix_f[2], mix_f[3], mix_f[4], mix_f[5], mix_f[6]);
            } else {
                for (auto &t: mix.mixes) {
                    if (t.weight < EPS) break;

                    float *src = ((LlamaAdapter *) model->adapter)->embeddings.data() +
                                 (t.token * n_embd);
                    float weight = t.weight;

                    for (size_t i = 0; i < n_embd; i++) {
                        mix_f[i] += src[i] * weight;
                    }

                    num_added++;
                }
            }

            if(num_added == 0){
                AKLOGE("Somehow a token mix had 0 weight for everything");
                ASSERT(false);
            }

            embeds.insert(embeds.end(), mix_f.begin(), mix_f.end());
            size++;
        }
        TIME_END(EmbedMixing)

        TIME_START(CachedMixAmount)
        int n_tokens = int32_t(mixes.size());
        int n_past = GetCachedMixAmount(mixes);
        past_mixes = mixes;

        if(!prompt_ff.first.empty()) n_past = 0; // We have to recompute embeds completely if prompt changed
        llama_kv_cache_seq_rm(ctx, 0, prompt.size() + n_past, -1);
        TIME_END(CachedMixAmount)

        if(!embeds.empty()) {
            TIME_START(DecodeEmbeds)
            // TODO: This is only processing one embd at a time, increasing n_tokens doesn't seem to work
            for(int h = n_past; h < n_tokens; h++ ) {
                llama_batch embd_batch = {
                        1,

                        nullptr,
                        embeds.data() + h*n_embd,
                        batch.pos,
                        batch.n_seq_id,
                        batch.seq_id,
                        batch.logits,

                        batch.all_pos_0,
                        batch.all_pos_1,
                        batch.all_seq_id
                };

                batch.pos[0] = prompt.size() + h;
                batch.seq_id[0][0] = 0;
                batch.n_seq_id[0] = 1;
                batch.logits[0] = false;

                if (llama_decode(ctx, embd_batch) != 0) {
                    AKLOGE("llama_decode() with embeds failed");
                    return {};
                }
            }
            TIME_END(DecodeEmbeds)

            TIME_START(DecodeXBC)

            // We always force an XBC token after
            size += 1;
            batch.n_tokens = 1;
            batch.token[0] = specialTokens.XBC;
            batch.seq_id[0][0] = 0;
            batch.n_seq_id[0] = 1;
            batch.logits[0] = true;
            batch.pos[0] = prompt.size() + n_tokens;
            head = 0;

            if (llama_decode(ctx, batch) != 0) {
                AKLOGE("llama_decode() for XBC failed");
                return {};
            }

            TIME_END(DecodeXBC)

            ASSERT(size == prompt.size() + n_tokens + 1);
            ASSERT(size == prompt.size() + (embeds.size() / n_embd) + 1);
        } else {
            ASSERT(size == prompt.size());
            ASSERT(head == prompt_ff.first.size() - 1);
        }

        AKLOGI("-- Decode");
        AKLOGI("First we processed the prompt (%d):", prompt_ff.first.size());
        for(auto t : prompt) {
            AKLOGI(" - [%s]", model->getToken(t));
        }
        AKLOGI("Then %d embeds (cached %d)", embeds.size(), n_past);
        AKLOGI("The final size is %d and head is %d", size, head);

        TIME_START(FinishRm)

        llama_kv_cache_seq_rm(ctx, 0, size, -1);

        TIME_END(FinishRm)
        return {
            head,
            size
        };
    }

    std::vector<std::pair<float, token_sequence>> Sample(DecodeResult decodeResult, int n_results) {
        llama_context *ctx = ((LlamaAdapter *) model->adapter)->context;
        llama_batch batch = ((LlamaAdapter *) model->adapter)->batch;

        size_t n_vocab = llama_n_vocab(llama_get_model(ctx));

        std::vector<potential_sequence> sequences;

        bool allow_correction_token = decodeResult.logits_head == 0;

        float *logits = llama_get_logits_ith(ctx, decodeResult.logits_head);
        transform_logits(logits, n_vocab, false, allow_correction_token);

        std::vector<std::pair<float, int>> index_value;
        index_value.clear();
        for (size_t i = 0; i < n_vocab; i++) {
            index_value.emplace_back(logits[i], i);
        }

        sortProbabilityPairVectorDescending(index_value, n_results);

        for (int i = 0; i < n_results; i++) {
            sequences.emplace_back(
                    index_value[i].first,
                    potential_sequence_data {
                            {index_value[i].second},
                            i
                    }
            );
        }

        for (auto &sequence: sequences) {
            if (sequence.second.seq_id == 0) continue;

            llama_kv_cache_seq_cp(ctx, 0, sequence.second.seq_id, 0, decodeResult.size);
        }

            std::vector<potential_sequence> next_sequences;

        std::vector<std::pair<float, token_sequence>> outputs;

        for(int tok=0; tok<10; tok++) {
            next_sequences.clear();
            for (auto sequence: std::move(sequences)) {
                int next_token = sequence.second.tokens[sequence.second.tokens.size() - 1];

                // Check if this is the end of correction
                if (next_token == specialTokens.XEC) {
                    token_sequence resulting_tokens = std::move(sequence.second.tokens);
                    resulting_tokens.resize(resulting_tokens.size() - 1);
                    outputs.emplace_back(sequence.first, resulting_tokens);
                    continue;
                }

                // Check if this is the end of a word
                std::string token = model->getToken(next_token);
                if (token.size() >= 3 && (token[token.size() - 1] == '\x81') &&
                    (token[token.size() - 2] == '\x96') && token[token.size() - 3] == '\xe2') {
                    outputs.emplace_back(sequence.first, std::move(sequence.second.tokens));
                    continue;
                }

                next_sequences.emplace_back(sequence);
            }

            sequences = next_sequences;
            next_sequences.clear();

            size_t remaining_count = n_results - outputs.size();
            batch.n_tokens = 0;

            //for(int i=0; i<batch.n_tokens; i++) batch.logits[i] = false;
            for (auto &sequence: sequences) {
                batch.token[batch.n_tokens] = sequence.second.tokens[sequence.second.tokens.size() - 1];
                batch.pos[batch.n_tokens] = decodeResult.size + (sequence.second.tokens.size() - 1);
                batch.seq_id[batch.n_tokens][0] = sequence.second.seq_id;
                batch.n_seq_id[batch.n_tokens] = 1;
                batch.logits[batch.n_tokens] = true;

                batch.n_tokens += 1;
            }

            ASSERT(batch.n_tokens == remaining_count); // usually 3

            if (batch.n_tokens == 0) {
                break;
            }

            llama_decode(ctx, batch);

            for (int seq = 0; seq < remaining_count; seq++) {
                const potential_sequence &parent_seq = sequences[seq];
                logits = llama_get_logits_ith(ctx, seq);
                transform_logits(logits, n_vocab, true, allow_correction_token);

                index_value.clear();
                for (size_t i = 0; i < n_vocab; i++) {
                    index_value.emplace_back(logits[i], i);
                }

                sortProbabilityPairVectorDescending(index_value, remaining_count);

                for (size_t i = 0; i < remaining_count; i++) {
                    token_sequence new_sequence = parent_seq.second.tokens;
                    new_sequence.push_back(index_value[i].second);

                    if (index_value[i].first > 1.0f || index_value[i].first < 0.0f) {
                        AKLOGE("Expected index_value to be probability [%.2f]",
                               index_value[i].first);
                    }

                    if (sequences[i].first > 1.0f || sequences[i].first < 0.0f) {
                        AKLOGE("Expected sequences value to be probability [%.2f]",
                               sequences[i].first);
                    }

                    next_sequences.emplace_back(
                            index_value[i].first * sequences[i].first,
                            potential_sequence_data{
                                    new_sequence,
                                    parent_seq.second.seq_id
                            }
                    );
                }
            }

            sortProbabilityPairVectorDescending(next_sequences, remaining_count);
            next_sequences.resize(remaining_count);
            sequences.clear();

            // In some cases we may have picked a sequence from the same parent sequence
            // We must re-assign the seq_id
            int seq_id_use_count[n_results];
            for (int i = 0; i < n_results; i++) seq_id_use_count[i] = 0;

            for (auto &seq: next_sequences) seq_id_use_count[seq.second.seq_id] += 1;

            for (auto &seq: next_sequences) {
                if (seq_id_use_count[seq.second.seq_id] > 1) {
                    int old_seq_id = seq.second.seq_id;

                    int new_seq_id = -1;
                    for (int i = 0; i < n_results; i++) {
                        if (seq_id_use_count[i] == 0) {
                            new_seq_id = i;
                            break;
                        }
                    }

                    if (new_seq_id == -1) {
                        AKLOGE("Couldn't find an empty sequence id to use. This should never happen.");
                        return {};
                    }

                    seq_id_use_count[old_seq_id]--;
                    seq_id_use_count[new_seq_id]++;

                    llama_kv_cache_seq_cp(
                            ctx,
                            old_seq_id,
                            new_seq_id,
                            0, // could start from prompt.size()
                            decodeResult.size + (seq.second.tokens.size() - 1)
                    );

                    seq.second.seq_id = new_seq_id;
                }
            }

            sequences = next_sequences;
        }

        for (int i = 1; i < n_results; i++) {
            llama_kv_cache_seq_rm(ctx, i, 0, -1);
        }

        return outputs;
    }

    std::vector<std::pair<float, std::string>> PredictNextWord(const std::string &context) {
        token_sequence next_context = model->tokenize(trim(context) + " ");
        next_context.insert(next_context.begin(), 1); // BOS

        auto decoding_result = DecodePromptAndMixes(next_context, { });
        auto results = Sample(decoding_result, 3);

        std::vector<std::pair<float, std::string>> str_results;
        for(const auto& result : results) {
            str_results.emplace_back(result.first, model->decode(result.second));
        }

        return str_results;
    }

    std::vector<std::pair<float, std::string>> PredictCorrection(const std::string &context, std::string &word, const std::vector<TokenMix> &mixes, bool swipe_mode) {
        token_sequence next_context;
        if(context.length() != 0) {
            next_context = model->tokenize(trim(context) + " ");
        }

        next_context.insert(next_context.begin(), 1); // BOS
        next_context.push_back(specialTokens.XBU);

        if(swipe_mode) {
            next_context.push_back(specialTokens.XC0_SWIPE_MODE);
        }

        auto decoding_result = DecodePromptAndMixes(next_context, mixes);
        auto results = Sample(decoding_result, 3);

        std::vector<std::pair<float, std::string>> str_results;
        for(const auto& result : results) {
            str_results.emplace_back(result.first, model->decode(result.second));
        }

        return str_results;
    }
};

namespace latinime {
    static jlong xlm_LanguageModel_open(JNIEnv *env, jclass clazz, jstring modelDir) {
        AKLOGI("open LM");
        const jsize sourceDirUtf8Length = env->GetStringUTFLength(modelDir);
        if (sourceDirUtf8Length <= 0) {
            AKLOGE("DICT: Can't get sourceDir string");
            return 0;
        }
        char sourceDirChars[sourceDirUtf8Length + 1];
        env->GetStringUTFRegion(modelDir, 0, env->GetStringLength(modelDir), sourceDirChars);
        sourceDirChars[sourceDirUtf8Length] = '\0';

        LanguageModelState *state = new LanguageModelState();

        if(!state->Initialize(sourceDirChars)) {
            delete state;
            return 0;
        }

        return reinterpret_cast<jlong>(state);
    }

    static void xlm_LanguageModel_close(JNIEnv *env, jclass clazz, jlong statePtr) {
        LanguageModelState *state = reinterpret_cast<LanguageModelState *>(statePtr);
        if(state == nullptr) return;
        delete state;
    }

    static void xlm_LanguageModel_getSuggestions(JNIEnv *env, jclass clazz,
         // inputs
         jlong dict,
         jlong proximityInfo,
         jstring context,
         jstring partialWord,
         jint inputMode,
         jintArray inComposeX,
         jintArray inComposeY,

         // outputs
         jobjectArray outPredictions,
         jfloatArray outProbabilities
    ) {
        LanguageModelState *state = reinterpret_cast<LanguageModelState *>(dict);
        ProximityInfo *pInfo = reinterpret_cast<ProximityInfo *>(proximityInfo);

        size_t inputSize = env->GetArrayLength(inComposeX);

        const char* cstr = env->GetStringUTFChars(context, nullptr);
        std::string contextString(cstr);
        env->ReleaseStringUTFChars(context, cstr);

        std::string partialWordString;
        if(partialWord != nullptr){
            const char* pwstr = env->GetStringUTFChars(partialWord, nullptr);
            partialWordString = std::string(pwstr);
            env->ReleaseStringUTFChars(partialWord, pwstr);
        }

        if(partialWordString.size() < inputSize) inputSize = partialWordString.size();

        TIME_START(GettingMixes)
        int xCoordinates[inputSize];
        int yCoordinates[inputSize];
        env->GetIntArrayRegion(inComposeX, 0, inputSize, xCoordinates);
        env->GetIntArrayRegion(inComposeY, 0, inputSize, yCoordinates);

        std::vector<TokenMix> mixes;
        for(int i=0; i<inputSize; i++) {
            std::vector<float> proportions = pInfo->decomposeTapPosition(xCoordinates[i], yCoordinates[i]);
            for(float &f : proportions) {
                if(f < 0.05f) f = 0.0f;
            }

            std::vector<std::pair<float, int>> index_value;
            index_value.clear();
            for (size_t k = 0; k < proportions.size(); k++) {
                index_value.emplace_back(proportions[k], k);
            }

            sortProbabilityPairVectorDescending(index_value, NUM_TOKEN_MIX);

            bool needs_resorting = false;
            int num_symbols = 0;
            for(int s=0; s<100; s++) {
                for (int j = 0; j < NUM_TOKEN_MIX; j++) {
                    char c = (char) (pInfo->getKeyCodePoint(index_value[j].second));

                    if (c >= 'a' && c <= 'z') {
                    } else if (c >= 'A' && c <= 'Z') {
                    } else {
                        index_value[j].first = -99999.0f;
                        needs_resorting = true;
                        num_symbols++;
                    }
                }
                if(num_symbols == NUM_TOKEN_MIX) break;
                if(!needs_resorting) break;
                sortProbabilityPairVectorDescending(index_value, NUM_TOKEN_MIX);
            }
            if(num_symbols == NUM_TOKEN_MIX) continue; // Skip the symbol character

            float total_sum = 0.0f;
            for(int j=0; j<NUM_TOKEN_MIX; j++) {
                total_sum += index_value[j].first;
            }

            if(total_sum == 0.0f) {
                AKLOGE("Oh crap");
            }

            for(int j=0; j<NUM_TOKEN_MIX; j++) {
                index_value[j].first /= total_sum;
            }

            TokenMix results;
            results.x = ((float)xCoordinates[i]) / ((float)pInfo->getKeyboardWidth());
            results.y = ((float)yCoordinates[i]) / ((float)pInfo->getKeyboardHeight());

            AKLOGI("%d | Char %c, pos %.6f %.6f, nearest is %c at %.2f, then %c at %.2f, finally %c at %.2f", i, partialWordString[i],
                   results.x, results.y,
                   (char)(pInfo->getKeyCodePoint(index_value[0].second)), (float)(index_value[0].first),
                   (char)(pInfo->getKeyCodePoint(index_value[1].second)), (float)(index_value[1].first),
                   (char)(pInfo->getKeyCodePoint(index_value[2].second)), (float)(index_value[2].first)
               );


            for(int j=0; j<NUM_TOKEN_MIX; j++) {
                char c = (char) (pInfo->getKeyCodePoint(index_value[j].second));
                float w = (float) (index_value[j].first);

                results.mixes[j].weight = w;
                if(c >= 'a' && c <= 'z') {
                    results.mixes[j].token = (state->specialTokens.LETTERS_TO_IDS[c - 'a']);
                }else if(c >= 'A' && c <= 'Z') {
                    results.mixes[j].token = (state->specialTokens.LETTERS_TO_IDS[c - 'A']);
                } else {
                    AKLOGI("ignoring character in partial word [%c]", c);
                    results.mixes[j].weight = 0.0f;
                }
            }

            mixes.push_back(results);
        }

        TIME_END(GettingMixes)

        //AKLOGI("LanguageModel context [%s]", contextString.c_str());

        bool isAutoCorrect = false;
        std::vector<std::pair<float, std::string>> results;
        if(partialWordString.empty()) {
            results = state->PredictNextWord(contextString);

            //for(const auto &result : results) {
            //    AKLOGI("LanguageModel suggestion %.2f [%s]", result.first, result.second.c_str());
            //}
        } else {
            isAutoCorrect = true;
            bool swipeMode = inputMode == 1;
            results = state->PredictCorrection(contextString, partialWordString, mixes, swipeMode);

            //for(const auto &result : results) {
            //    AKLOGI("LanguageModel correction %.2f [%s] -> [%s]", result.first, partialWordString.c_str(), result.second.c_str());
            //}
        }

        // Output
        size_t size = env->GetArrayLength(outPredictions);

        jfloat *probsArray = env->GetFloatArrayElements(outProbabilities, nullptr);

        // Output predictions for next word
        for (int i = 0; i < results.size(); i++) {
            jstring jstr = env->NewStringUTF(results[i].second.c_str());
            env->SetObjectArrayElement(outPredictions, i, jstr);
            probsArray[i] = results[i].first;
            env->DeleteLocalRef(jstr);
        }

        env->ReleaseFloatArrayElements(outProbabilities, probsArray, 0);
    }

    static const JNINativeMethod sMethods[] = {
            {
                    const_cast<char *>("openNative"),
                    const_cast<char *>("(Ljava/lang/String;)J"),
                    reinterpret_cast<void *>(xlm_LanguageModel_open)
            },
            {
                    const_cast<char *>("closeNative"),
                    const_cast<char *>("(J)V"),
                    reinterpret_cast<void *>(xlm_LanguageModel_close)
            },
            {
                    const_cast<char *>("getSuggestionsNative"),
                    const_cast<char *>("(JJLjava/lang/String;Ljava/lang/String;I[I[I[Ljava/lang/String;[F)V"),
                    reinterpret_cast<void *>(xlm_LanguageModel_getSuggestions)
            }
    };


    static void llama_log_callback(ggml_log_level level, const char * text, void * user_data) {
        switch(level) {
            case GGML_LOG_LEVEL_ERROR:
                AKLOGE("llama err:  %s", text);
                break;
            case GGML_LOG_LEVEL_WARN:
                AKLOGI("llama warn: %s", text);
                break;
            case GGML_LOG_LEVEL_INFO:
                AKLOGI("llama info: %s", text);
                break;
        }
    }

    int register_LanguageModel(JNIEnv *env) {
        llama_backend_init(true /* numa??? */);
        llama_log_set(llama_log_callback, nullptr);

        const char *const kClassPathName = "org/futo/inputmethod/latin/xlm/LanguageModel";
        return registerNativeMethods(env, kClassPathName, sMethods, NELEMS(sMethods));
    }
} // namespace latinime
