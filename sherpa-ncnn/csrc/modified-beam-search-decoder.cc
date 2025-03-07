/**
 * Copyright (c)  2022  Xiaomi Corporation (authors: Fangjun Kuang)
 * Copyright (c)  2022                     (Pingfeng Luo)
 *
 * See LICENSE for clarification regarding multiple authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sherpa-ncnn/csrc/modified-beam-search-decoder.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "sherpa-ncnn/csrc/math.h"

namespace sherpa_ncnn {

// @param in 1-D tensor of shape (encoder_dim,)
// @param n Number of times to repeat
// @return Return a 2-d tensor of shape (n, encoder_dim)
//
// TODO(fangjun): Remove this function
// once
// https://github.com/nihui/ncnn/tree/pnnx-ncnn-binary-broadcast
// gets merged
static ncnn::Mat RepeatEncoderOut(ncnn::Mat in, int32_t n) {
  int32_t w = in.w;
  ncnn::Mat out(w, n, sizeof(float));

  const float *in_ptr = in;
  float *out_ptr = out;

  for (int32_t i = 0; i != n; ++i) {
    std::copy(in_ptr, in_ptr + w, out_ptr);
    out_ptr += w;
  }

  return out;
}

DecoderResult ModifiedBeamSearchDecoder::GetEmptyResult() const {
  DecoderResult r;

  int32_t context_size = model_->ContextSize();
  int32_t blank_id = 0;  // always 0

  std::vector<int32_t> blanks(context_size, blank_id);
  Hypotheses blank_hyp({{blanks, 0}});

  r.hyps = std::move(blank_hyp);

  return r;
}

void ModifiedBeamSearchDecoder::StripLeadingBlanks(DecoderResult *r) const {
  int32_t context_size = model_->ContextSize();
  auto hyp = r->hyps.GetMostProbable(true);

  auto start = hyp.ys.begin() + context_size;
  auto end = hyp.ys.end();

  r->tokens = std::vector<int32_t>(start, end);
  r->timestamps = std::move(hyp.timestamps);
  r->num_trailing_blanks = hyp.num_trailing_blanks;
}

// Compute log_softmax in-place.
//
// The log_softmax of each row is computed.
//
// @param in_out A 2-D tensor
static void LogSoftmax(ncnn::Mat *in_out) {
  int32_t h = in_out->h;
  int32_t w = in_out->w;
  for (int32_t y = 0; y != h; ++y) {
    float *p = in_out->row(y);
    LogSoftmax(p, w);
  }
}

// The decoder model contains an embedding layer, which only supports
// 1-D output.
// This is a wrapper to support 2-D decoder output.
//
// @param model_ The NN model.
// @param decoder_input A 2-D tensor of shape (num_active_paths, context_size)
// @return Return a 2-D tensor of shape (num_active_paths, decoder_dim)
//
// TODO(fangjun): Change Embed in ncnn to output 2-d tensors
static ncnn::Mat RunDecoder2D(Model *model_, ncnn::Mat decoder_input) {
  ncnn::Mat decoder_out;
  int32_t h = decoder_input.h;

  for (int32_t y = 0; y != h; ++y) {
    ncnn::Mat decoder_input_t =
        ncnn::Mat(decoder_input.w, decoder_input.row(y));

    ncnn::Mat tmp = model_->RunDecoder(decoder_input_t);

    if (y == 0) {
      decoder_out = ncnn::Mat(tmp.w, h);
    }

    const float *ptr = tmp;
    float *out_ptr = decoder_out.row(y);
    std::copy(ptr, ptr + tmp.w, out_ptr);
  }

  return decoder_out;
}

ncnn::Mat ModifiedBeamSearchDecoder::BuildDecoderInput(
    const std::vector<Hypothesis> &hyps) const {
  int32_t num_hyps = static_cast<int32_t>(hyps.size());
  int32_t context_size = model_->ContextSize();

  ncnn::Mat decoder_input(context_size, num_hyps);
  auto p = static_cast<int32_t *>(decoder_input);

  for (const auto &hyp : hyps) {
    const auto &ys = hyp.ys;
    std::copy(ys.end() - context_size, ys.end(), p);
    p += context_size;
  }

  return decoder_input;
}

void ModifiedBeamSearchDecoder::Decode(ncnn::Mat encoder_out,
                                       DecoderResult *result) {
  int32_t context_size = model_->ContextSize();
  Hypotheses cur = std::move(result->hyps);
  /* encoder_out.w == encoder_out_dim, encoder_out.h == num_frames. */
  for (int32_t t = 0; t != encoder_out.h; ++t) {
    std::vector<Hypothesis> prev = cur.GetTopK(num_active_paths_, true);
    cur.Clear();

    ncnn::Mat decoder_input = BuildDecoderInput(prev);
    ncnn::Mat decoder_out;
    if (t == 0 && prev.size() == 1 && prev[0].ys.size() == context_size &&
        !result->decoder_out.empty()) {
      // When an endpoint is detected, we keep the decoder_out
      decoder_out = result->decoder_out;
    } else {
      decoder_out = RunDecoder2D(model_, decoder_input);
    }

    // decoder_out.w == decoder_dim
    // decoder_out.h == num_active_paths
    ncnn::Mat encoder_out_t(encoder_out.w, encoder_out.row(t));
    encoder_out_t = RepeatEncoderOut(encoder_out_t, decoder_out.h);

    ncnn::Mat joiner_out = model_->RunJoiner(encoder_out_t, decoder_out);
    // joiner_out.w == vocab_size
    // joiner_out.h == num_active_paths
    LogSoftmax(&joiner_out);
    auto topk = TopkIndex(static_cast<float *>(joiner_out),
                          joiner_out.w * joiner_out.h, num_active_paths_);

    int32_t frame_offset = result->frame_offset;
    for (auto i : topk) {
      int32_t hyp_index = i / joiner_out.w;
      int32_t new_token = i % joiner_out.w;

      const float *p = joiner_out.row(hyp_index);

      Hypothesis new_hyp = prev[hyp_index];

      // blank id is fixed to 0
      if (new_token != 0) {
        new_hyp.ys.push_back(new_token);
        new_hyp.num_trailing_blanks = 0;
        new_hyp.timestamps.push_back(t + frame_offset);
      } else {
        ++new_hyp.num_trailing_blanks;
      }
      new_hyp.log_prob += p[new_token];
      cur.Add(std::move(new_hyp));
    }
  }

  result->hyps = std::move(cur);
  result->frame_offset += encoder_out.h;
  auto hyp = result->hyps.GetMostProbable(true);

  // set decoder_out in case of endpointing
  ncnn::Mat decoder_input = BuildDecoderInput({hyp});
  result->decoder_out = model_->RunDecoder(decoder_input);

  result->tokens = std::move(hyp.ys);
  result->num_trailing_blanks = hyp.num_trailing_blanks;
}

}  // namespace sherpa_ncnn
