#include "FaceInferOutcomeJson.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace {

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            static const char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(kHex[(c >> 4) & 0x0f]);
            out.push_back(kHex[c & 0x0f]);
        } else if (c == 0x7f) {
            out += "\\u007f";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

class JsonWriter {
public:
    JsonWriter() { out_.reserve(4096); }
    void beginObject() {
        beginValue_();
        out_ += "{";
        ctx_.push_back(CtxType::Object);
        first_.push_back(true);
    }
    void endObject() {
        out_ += "}";
        if (!ctx_.empty()) ctx_.pop_back();
        if (!first_.empty()) first_.pop_back();
    }

    void beginArray() {
        beginValue_();
        out_ += "[";
        ctx_.push_back(CtxType::Array);
        first_.push_back(true);
    }
    void endArray() {
        out_ += "]";
        if (!ctx_.empty()) ctx_.pop_back();
        if (!first_.empty()) first_.pop_back();
    }

    void key(const char* k) {
        if (!first_.empty()) {
            if (!first_.back()) out_ += ",";
            first_.back() = false;
        }
        out_ += "\""; out_ += jsonEscape(k); out_ += "\":";
    }

    void string(const std::string& v) {
        beginValue_();
        out_ += "\""; out_ += jsonEscape(v); out_ += "\"";
    }
    void string(const char* v) {
        beginValue_();
        out_ += "\""; out_ += jsonEscape(v ? std::string(v) : std::string()); out_ += "\"";
    }
    void boolean(bool v) {
        beginValue_();
        out_ += (v ? "true" : "false");
    }
    void number(double v) {
        beginValue_();
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v);
        out_ += buf;
    }
    void number(long long v) {
        beginValue_();
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", v);
        out_ += buf;
    }
    void number(std::uint64_t v) {
        beginValue_();
        out_ += std::to_string(v);
    }

    std::string str() const { return out_; }

private:
    enum class CtxType { Object, Array };

    void beginValue_() {
        if (ctx_.empty() || first_.empty()) return;
        if (ctx_.size() != first_.size()) return;
        if (ctx_.back() == CtxType::Array) {
            if (!first_.back()) out_ += ",";
            first_.back() = false;
        }
    }

    std::string out_;
    std::vector<CtxType> ctx_;
    std::vector<bool> first_;
};

static void writeRequestJson(JsonWriter& j, const FaceInferRequest& req) {
    j.key("request");
    j.beginObject();
    j.key("yolo");
    j.beginObject();
    j.key("backend");
    j.string(req.yoloBackend);
    j.key("modelPath");
    j.string(req.yoloModelPath);
    j.key("configPath");
    j.string(req.yoloConfigPath);
    j.key("framework");
    j.string(req.yoloFramework);
    j.key("outputName");
    j.string(req.yoloOutputName);
    j.key("inputW");
    j.number(static_cast<long long>(req.yoloInputW));
    j.key("inputH");
    j.number(static_cast<long long>(req.yoloInputH));
    j.key("scoreThreshold");
    j.number(static_cast<double>(req.yoloScoreThreshold));
    j.key("nmsIouThreshold");
    j.number(static_cast<double>(req.yoloNmsIouThreshold));
    j.key("enableKeypoints5");
    j.boolean(req.yoloEnableKeypoints5);
    j.key("letterbox");
    j.boolean(req.yoloLetterbox);
    j.key("swapRB");
    j.boolean(req.yoloSwapRB);
    j.key("scale");
    j.number(static_cast<double>(req.yoloScale));
    j.key("meanB");
    j.number(static_cast<long long>(req.yoloMeanB));
    j.key("meanG");
    j.number(static_cast<long long>(req.yoloMeanG));
    j.key("meanR");
    j.number(static_cast<long long>(req.yoloMeanR));
    j.key("opencvBackend");
    j.number(static_cast<long long>(req.yoloOpenCvBackend));
    j.key("opencvTarget");
    j.number(static_cast<long long>(req.yoloOpenCvTarget));
    j.endObject();
    j.key("arc");
    j.beginObject();
    j.key("backend");
    j.string(req.arcBackend);
    j.key("modelPath");
    j.string(req.arcModelPath);
    j.key("configPath");
    j.string(req.arcConfigPath);
    j.key("framework");
    j.string(req.arcFramework);
    j.key("outputName");
    j.string(req.arcOutputName);
    j.key("inputName");
    j.string(req.arcInputName);
    j.key("inputW");
    j.number(static_cast<long long>(req.arcInputW));
    j.key("inputH");
    j.number(static_cast<long long>(req.arcInputH));
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(req.arcModelVersion));
    j.key("preprocessVersion");
    j.number(static_cast<std::uint64_t>(req.arcPreprocessVersion));
    j.key("fakeEmbedding");
    j.boolean(req.fakeEmbedding);
    j.endObject();
    j.key("gallery");
    j.beginObject();
    j.key("dir");
    j.string(req.galleryDir);
    j.key("topK");
    j.number(static_cast<std::uint64_t>(req.topK));
    j.endObject();
    j.key("threshold");
    j.beginObject();
    j.key("acceptThreshold");
    j.number(static_cast<double>(req.acceptThreshold));
    j.key("versionId");
    j.string(req.thresholdVersionId);
    j.key("consecutivePassesToTrigger");
    j.number(static_cast<long long>(req.consecutivePassesToTrigger));
    j.endObject();
    j.key("faceSelectPolicy");
    j.string(req.faceSelectPolicy);
    j.key("fakeDetect");
    j.boolean(req.fakeDetect);
    j.endObject();
}

}  // namespace

std::string buildImageLoadFailureJson(const FaceInferOutcomeJsonInput& in) {
    if (!in.metrics) return "{}";

    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(false);
    j.key("errorCode");
    j.number(static_cast<long long>(in.out.errorCode));
    j.key("stage");
    j.string(in.out.stage);
    j.key("message");
    j.string(in.out.message);
    j.key("timestamp_ms");
    j.number(in.timestampMs);
    j.key("image");
    j.string(in.req.imagePath);
    j.key("frame");
    j.beginObject();
    j.key("w");
    j.number(0LL);
    j.key("h");
    j.number(0LL);
    j.endObject();
    j.key("metrics");
    j.beginObject();
    j.key("msLoad");
    j.number(in.metrics->msLoad);
    j.key("msTotal");
    j.number(in.metrics->msTotal);
    j.endObject();
    writeRequestJson(j, in.req);
    j.endObject();
    return j.str();
}

std::string buildOutcomeJson(const FaceInferOutcomeJsonInput& in) {
    if (!in.ctx || !in.decision || !in.metrics) return "{}";

    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(in.out.ok);
    j.key("errorCode");
    j.number(static_cast<long long>(in.out.errorCode));
    j.key("stage");
    j.string(in.out.stage);
    j.key("message");
    j.string(in.out.message);
    j.key("timestamp_ms");
    j.number(in.timestampMs);
    j.key("image");
    j.string(in.req.imagePath);
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(in.req.arcModelVersion));
    j.key("thresholdVersion");
    j.string(in.decision->versionId);
    j.key("frame");
    j.beginObject();
    j.key("w");
    j.number(static_cast<long long>(in.ctx->img.cols));
    j.key("h");
    j.number(static_cast<long long>(in.ctx->img.rows));
    j.endObject();
    j.key("yolo");
    j.beginObject();
    j.key("backend");
    j.string(in.ctx->yoloBackendName.empty() ? in.req.yoloBackend : in.ctx->yoloBackendName);
    j.key("fake");
    j.boolean(in.req.fakeDetect);
    j.endObject();
    j.key("face");
    j.beginObject();
    j.key("hasFace");
    j.boolean(in.ctx->hasFace);
    if (in.ctx->hasFace) {
        j.key("bbox");
        j.beginObject();
        j.key("x");
        j.number(static_cast<double>(in.ctx->mainFace.bbox.x));
        j.key("y");
        j.number(static_cast<double>(in.ctx->mainFace.bbox.y));
        j.key("w");
        j.number(static_cast<double>(in.ctx->mainFace.bbox.width));
        j.key("h");
        j.number(static_cast<double>(in.ctx->mainFace.bbox.height));
        j.endObject();
        j.key("score");
        j.number(static_cast<double>(in.ctx->mainFace.score));
        j.key("hasKeypoints5");
        j.boolean(in.ctx->mainFace.keypoints5.has_value());
        j.key("align");
        j.beginObject();
        j.key("usedKeypoints5");
        j.boolean(in.ctx->usedKeypoints5);
        j.key("usedBboxFallback");
        j.boolean(in.ctx->usedBboxFallback);
        j.key("outW");
        j.number(static_cast<long long>(in.req.arcInputW));
        j.key("outH");
        j.number(static_cast<long long>(in.req.arcInputH));
        j.endObject();
    }
    j.endObject();
    j.key("embedding");
    j.beginObject();
    j.key("dim");
    j.number(512LL);
    j.key("backend");
    j.string(in.ctx->arcBackendName);
    j.key("fake");
    j.boolean(in.ctx->embeddingFake);
    j.key("modelVersion");
    j.number(static_cast<std::uint64_t>(in.req.arcModelVersion));
    j.key("preprocessVersion");
    j.number(static_cast<std::uint64_t>(in.req.arcPreprocessVersion));
    j.endObject();
    j.key("gallery");
    j.beginObject();
    j.key("dir");
    j.string(in.req.galleryDir);
    j.key("size");
    j.number(static_cast<std::uint64_t>((in.ctx->hasFace && in.ctx->embeddingOk) ? in.ctx->index.size() : 0));
    if (!in.ctx->galleryWarnings.empty()) {
        j.key("warnings");
        j.beginArray();
        for (const auto& w : in.ctx->galleryWarnings) j.string(w);
        j.endArray();
    }
    j.endObject();
    j.key("TopK");
    j.beginObject();
    j.key("k");
    j.number(static_cast<std::uint64_t>(in.req.topK));
    j.key("hits");
    j.beginArray();
    for (size_t i = 0; i < in.ctx->hits.size(); i++) {
        j.beginObject();
        j.key("rank");
        j.number(static_cast<long long>(i + 1));
        j.key("id");
        j.string(in.ctx->hits[i].id);
        j.key("index");
        j.number(static_cast<long long>(in.ctx->hits[i].index));
        j.key("score");
        j.number(static_cast<double>(in.ctx->hits[i].score));
        j.endObject();
    }
    j.endArray();
    j.endObject();
    j.key("decision");
    j.beginObject();
    j.key("threshold");
    j.number(static_cast<double>(in.decision->threshold));
    j.key("thresholdVersion");
    j.string(in.decision->versionId);
    j.key("passNow");
    j.boolean(in.decision->passNow);
    j.key("triggeredNow");
    j.boolean(in.decision->triggeredNow);
    j.key("triggeredLatched");
    j.boolean(in.decision->triggeredLatched);
    j.key("passStreak");
    j.number(static_cast<std::uint64_t>(in.decision->passStreak));
    j.key("bestScore");
    j.number(static_cast<double>(in.decision->score));
    j.endObject();
    j.key("metrics");
    j.beginObject();
    j.key("msLoad");
    j.number(in.metrics->msLoad);
    j.key("msDetect");
    j.number(in.metrics->msDetect);
    j.key("msAlign");
    j.number(in.metrics->msAlign);
    j.key("msEmbed");
    j.number(in.metrics->msEmbed);
    j.key("msSearch");
    j.number(in.metrics->msSearch);
    j.key("msTotal");
    j.number(in.metrics->msTotal);
    j.endObject();
    writeRequestJson(j, in.req);
    j.endObject();
    return j.str();
}

std::string buildExceptionJson(const FaceInferOutcomeJsonInput& in) {
    JsonWriter j;
    j.beginObject();
    j.key("ok");
    j.boolean(false);
    j.key("errorCode");
    j.number(static_cast<long long>(in.out.errorCode));
    j.key("stage");
    j.string("exception");
    j.key("message");
    j.string(in.out.message);
    j.key("timestamp_ms");
    j.number(in.timestampMs);
    j.key("image");
    j.string(in.req.imagePath);
    j.endObject();
    return j.str();
}

