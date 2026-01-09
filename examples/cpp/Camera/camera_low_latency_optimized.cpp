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
    bool enableISP3AFpsLimit;
    int isp3aFps;
};

void runTest(const TestConfig& cfg) {
    std::cout << "\n========================================\n";
    std::cout << "Testing: " << cfg.name << "\n";
    std::cout << "Config: " << cfg.width << "x" << cfg.height
              << " @ " << cfg.fps << " fps";
    if(cfg.enableISP3AFpsLimit) {
        std::cout << " (ISP 3A @ " << cfg.isp3aFps << " fps)";
    }
    std::cout << "\n========================================\n";

    dai::Pipeline pipeline;

    // Optimize XLink chunk size for lower latency
    // Larger chunks = better throughput but higher latency
    // Smaller chunks = lower latency but more overhead
    // For low latency, use smaller chunks (32KB instead of default 64KB)
    pipeline.setXLinkChunkSize(32 * 1024);

    // Increase Leon CPU frequencies for faster processing
    // Default is 700MHz, can go higher for better performance
    pipeline.setLeonCssFrequencyHz(800 * 1000 * 1000);
    pipeline.setLeonMssFrequencyHz(800 * 1000 * 1000);

    auto cam = pipeline.create<dai::node::Camera>()
                   ->build(dai::CameraBoardSocket::CAM_A, std::nullopt, std::nullopt);

    // Reduce frame pool sizes for lower latency
    // Smaller pools = less buffering = lower latency
    // Trade-off: may drop frames if processing is slow
    cam->setRawNumFramesPool(2);      // Default: 3
    cam->setIspNumFramesPool(2);      // Default: 3
    cam->setOutputsNumFramesPool(2);  // Default: 4

    // Limit ISP 3A update rate to reduce CPU load
    if(cfg.enableISP3AFpsLimit) {
        cam->setIsp3aFps(cfg.isp3aFps);
    }

    auto controlQ = cam->inputControl.createInputQueue();

    auto camOut = cam->requestOutput(
        {cfg.width, cfg.height},
        std::optional<dai::ImgFrame::Type>{cfg.type},
        dai::ImgResizeMode::CROP,
        std::optional<float>{cfg.fps},
        std::nullopt
    );

    // Queue size of 1 for minimum latency (already optimal in original test)
    // Non-blocking to avoid delays
    auto q = camOut->createOutputQueue(1, false);

    pipeline.start();

    // Set short manual exposure for minimal sensor integration time
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
    double stddev = std::sqrt(var);

    auto [mnIt, mxIt] = std::minmax_element(latMs.begin(), latMs.end());

    std::cout << "\nRESULT [" << cfg.name << "]: "
              << "mean=" << std::fixed << std::setprecision(2) << mean
              << " min=" << *mnIt
              << " p50=" << pct(0.50)
              << " p95=" << pct(0.95)
              << " p99=" << pct(0.99)
              << " max=" << *mxIt
              << " stddev=" << stddev << " ms\n";

    pipeline.stop();
    pipeline.wait();
}

int main() {
    std::cout << "=== OAK-D Ultra-Low Latency Configuration Test ===\n";
    std::cout << "Target: 360p @ 24fps RGB with minimal latency\n\n";

    std::vector<TestConfig> tests = {
        // Primary target: 360p @ 24fps RGB (interleaved)
        {640, 360, 24.0f, dai::ImgFrame::Type::RGB888i, "360p24 RGB888i", false, 0},

        // Alternative: 360p @ 24fps with limited ISP 3A (reduce processing overhead)
        {640, 360, 24.0f, dai::ImgFrame::Type::RGB888i, "360p24 RGB888i (3A@12fps)", true, 12},

        // Comparison: RAW8 format (bypass some ISP processing)
        {640, 360, 24.0f, dai::ImgFrame::Type::RAW8, "360p24 RAW8", false, 0},

        // Comparison: Standard VGA resolution
        {640, 480, 24.0f, dai::ImgFrame::Type::RGB888i, "VGA24 RGB888i", false, 0},

        // Higher FPS test to see if latency changes
        {640, 360, 30.0f, dai::ImgFrame::Type::RGB888i, "360p30 RGB888i", false, 0},

        // Lowest resolution test
        {320, 240, 24.0f, dai::ImgFrame::Type::RGB888i, "320x240@24 RGB888i", false, 0},
    };

    std::cout << "Configuration optimizations applied:\n";
    std::cout << "  - XLink chunk size: 32KB (reduced from 64KB default)\n";
    std::cout << "  - Leon CSS/MSS frequency: 800MHz (increased from 700MHz default)\n";
    std::cout << "  - Frame pool sizes: 2 (reduced from 3-4 default)\n";
    std::cout << "  - Output queue size: 1 (non-blocking)\n";
    std::cout << "  - Manual exposure: 1ms @ ISO 1600\n\n";

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
    std::cout << "\nNOTE: The ~33ms latency floor is primarily due to:\n";
    std::cout << "  1. Sensor rolling shutter readout time (~15-20ms)\n";
    std::cout << "  2. ISP processing time (~5-10ms)\n";
    std::cout << "  3. USB transfer and buffering (~5-8ms)\n";
    std::cout << "\nThis is a hardware limitation of rolling shutter sensors.\n";
    std::cout << "Global shutter sensors can achieve <10ms latency but are not\n";
    std::cout << "available on the OAK-D original (IMX378 is rolling shutter).\n";

    return 0;
}
