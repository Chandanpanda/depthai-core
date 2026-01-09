#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

#include "depthai/depthai.hpp"

static constexpr int WARMUP_FRAMES = 30;
static constexpr int MEASURE_FRAMES = 300;

struct TestConfig {
    int width;
    int height;
    float fps;
    dai::ImgFrame::Type type;
    std::string name;
};

void runTest(const TestConfig& cfg) {
    std::cout << "\n========================================\n";
    std::cout << "Testing: " << cfg.name << "\n";
    std::cout << "Config: " << cfg.width << "x" << cfg.height 
              << " @ " << cfg.fps << " fps\n";
    std::cout << "========================================\n";

    dai::Pipeline pipeline;

    auto cam = pipeline.create<dai::node::Camera>()
                   ->build(dai::CameraBoardSocket::CAM_A, std::nullopt, std::nullopt);

    auto controlQ = cam->inputControl.createInputQueue();

    auto camOut = cam->requestOutput(
        {cfg.width, cfg.height},
        std::optional<dai::ImgFrame::Type>{cfg.type},
        dai::ImgResizeMode::CROP,
        std::optional<float>{cfg.fps},
        std::nullopt
    );

    auto q = camOut->createOutputQueue(1, false);

    pipeline.start();

    // Set short manual exposure
    {
        auto ctrl = std::make_shared<dai::CameraControl>();
        ctrl->setManualExposure(1000, 1600);  // 1ms exposure, ISO 1600
        controlQ->send(ctrl);
    }

    std::vector<double> latMs;
    latMs.reserve(MEASURE_FRAMES);

    int warmupLeft = WARMUP_FRAMES;
    bool printedFrameInfo = false;
    auto prevTs = std::chrono::steady_clock::now();

    while(pipeline.isRunning()) {
        auto frame = q->get<dai::ImgFrame>();
        if(!frame) continue;

        const auto hostNow = std::chrono::steady_clock::now();

        if(!printedFrameInfo) {
            std::cout << "Actual: " << frame->getWidth() << "x" << frame->getHeight()
                      << " | Type: " << static_cast<int>(frame->getType())
                      << " | Exposure: " << frame->getExposureTime().count() << " us"
                      << " | Data size: " << frame->getData().size() << " bytes\n";
            printedFrameInfo = true;
            prevTs = hostNow;
            continue;
        }

        const auto ts = frame->getTimestamp();
        const double latencyMs = std::chrono::duration<double, std::milli>(hostNow - ts).count();

        if(warmupLeft > 0) {
            --warmupLeft;
            prevTs = hostNow;
            continue;
        }

        latMs.push_back(latencyMs);

        double frameIntervalMs = std::chrono::duration<double, std::milli>(hostNow - prevTs).count();
        prevTs = hostNow;

        // Print every 100 frames
        if(latMs.size() % 100 == 0) {
            double avg = std::accumulate(latMs.begin(), latMs.end(), 0.0) / latMs.size();
            double instantFps = 1000.0 / frameIntervalMs;
            std::cout << "Frame " << latMs.size() 
                      << " | Latency: " << std::fixed << std::setprecision(2) << latencyMs
                      << " ms | Avg: " << avg
                      << " ms | FPS: " << instantFps << "\n";
        }

        if((int)latMs.size() >= MEASURE_FRAMES) break;
    }

    if(latMs.empty()) {
        std::cerr << "No frames measured!\n";
        pipeline.stop();
        pipeline.wait();
        return;
    }

    std::vector<double> sorted = latMs;
    std::sort(sorted.begin(), sorted.end());

    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
        return sorted[idx];
    };

    double mean = std::accumulate(latMs.begin(), latMs.end(), 0.0) / latMs.size();
    double var = 0.0;
    for(double x : latMs) { double d = x - mean; var += d * d; }
    var /= latMs.size();

    auto [mnIt, mxIt] = std::minmax_element(latMs.begin(), latMs.end());

    std::cout << "\nRESULT [" << cfg.name << "]: "
              << "mean=" << std::fixed << std::setprecision(2) << mean
              << " min=" << *mnIt
              << " p50=" << pct(0.50)
              << " p99=" << pct(0.99)
              << " max=" << *mxIt << " ms\n";

    pipeline.stop();
    pipeline.wait();
}

int main() {
    std::vector<TestConfig> tests = {
        // NV12 tests (with ISP)
        {1920, 1080, 60.0f, dai::ImgFrame::Type::NV12, "1080p60 NV12"},
        {1352, 1012, 52.0f, dai::ImgFrame::Type::NV12, "1352x1012@52 NV12"},
        
        // RAW tests (bypass ISP) - try different RAW formats
        {1920, 1080, 60.0f, dai::ImgFrame::Type::RAW8, "1080p60 RAW8"},
        {1352, 1012, 52.0f, dai::ImgFrame::Type::RAW8, "1352x1012@52 RAW8"},
        
        // Smaller resolution
        {640, 480, 60.0f, dai::ImgFrame::Type::NV12, "VGA@60 NV12"},
    };

    std::cout << "=== OAK-D Latency Test Suite ===\n";
    std::cout << "Testing multiple configurations to find lowest latency...\n";

    std::vector<std::pair<std::string, double>> results;

    for(const auto& test : tests) {
        try {
            runTest(test);
        } catch(const std::exception& e) {
            std::cerr << "Test [" << test.name << "] failed: " << e.what() << "\n";
        }
        
        // Small delay between tests
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n=== All Tests Complete ===\n";
    
    return 0;
}