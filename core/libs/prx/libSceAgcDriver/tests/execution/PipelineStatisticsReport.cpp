#include "VulkanTestDevice.hpp"
#include "../../tools/ComputePipelineFixture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;

class StderrCapture {
public:
    StderrCapture() : path(std::filesystem::temp_directory_path() / ("aps5_pipeline_statistics_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")) {
        std::fflush(stderr);
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        Require(saved >= 0, "cannot save stderr");
        Require(_wfreopen(path.wstring().c_str(), L"wb", stderr) != nullptr, "cannot redirect stderr");
#else
        saved = dup(fileno(stderr));
        Require(saved >= 0, "cannot save stderr");
        Require(std::freopen(path.c_str(), "wb", stderr) != nullptr, "cannot redirect stderr");
#endif
    }

    ~StderrCapture() {
        restore();
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string Finish() {
        restore();
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

private:
    void restore() {
        if (saved < 0) return;
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        saved = -1;
    }

    std::filesystem::path path;
    int saved = -1;
};

std::unique_ptr<AgcDriver::VulkanDevice> OpenDevice() {
    try {
        return OpenVulkanTestDevice();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find("APS5_PIPELINE_STATS requires") == std::string_view::npos) throw;
        std::printf("skipped, the device reports no pipeline executable statistics: %s\n", error.what());
        return nullptr;
    }
}

struct StatisticsPipeline {
    explicit StatisticsPipeline(const AgcDriver::Graphics::Context& context) : context(context) {
        const auto spirv = AgcDriver::ComputePipelineFixture().spirv;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = spirv.data();
        Require(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module) == VK_SUCCESS, "vkCreateShaderModule");
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        Require(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout) == VK_SUCCESS, "vkCreatePipelineLayout");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout;
        Require(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS, "vkCreateComputePipelines");
    }

    ~StatisticsPipeline() {
        context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    }

    StatisticsPipeline(const StatisticsPipeline&) = delete;
    StatisticsPipeline& operator=(const StatisticsPipeline&) = delete;

    AgcDriver::Graphics::Context context;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

}

int main() {
    try {
#ifdef _WIN32
        Require(_putenv_s("APS5_PIPELINE_STATS", "1") == 0, "cannot set APS5_PIPELINE_STATS");
#else
        Require(setenv("APS5_PIPELINE_STATS", "1", 1) == 0, "cannot set APS5_PIPELINE_STATS");
#endif
        const auto device = OpenDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        Require(context.pipelineExecutableInfo, "APS5_PIPELINE_STATS=1 did not enable pipeline executable statistics");
        const StatisticsPipeline pipeline(context);
        constexpr unsigned threads = 8;
        constexpr unsigned reportsPerThread = 64;
        std::vector<std::exception_ptr> failures(threads);
        StderrCapture output;
        {
            std::vector<std::jthread> workers;
            for (unsigned thread = 0; thread < threads; ++thread) {
                workers.emplace_back([&, thread] {
                    try {
                        for (unsigned report = 0; report < reportsPerThread; ++report) AgcDriver::Graphics::LogPipelineStatistics_nid_no_patch(context, pipeline.pipeline);
                    } catch (...) {
                        failures[thread] = std::current_exception();
                    }
                });
            }
        }
        const auto text = output.Finish();
        for (const auto& failure : failures) {
            if (failure) std::rethrow_exception(failure);
        }
        std::istringstream stream(text);
        std::vector<std::string> lines;
        for (std::string line; std::getline(stream, line);) {
            if (line.find("[pipeline-stats]") == std::string::npos) continue;
            Require(line.starts_with("[pipeline-stats] executable=") && line.find("[pipeline-stats]", 1) == std::string::npos, "a [pipeline-stats] line was split by another report");
            lines.push_back(std::move(line));
        }
        const auto header = [](const std::string& line) { return line.find(" stages=0x") != std::string::npos; };
        std::size_t reports = 0;
        std::size_t perReport = 0;
        for (std::size_t index = 0; index < lines.size();) {
            Require(header(lines[index]), "a statistic line was not preceded by its executable's header");
            std::size_t next = index + 1;
            while (next < lines.size() && !header(lines[next])) ++next;
            if (reports == 0) perReport = next - index - 1;
            Require(perReport != 0 && next - index - 1 == perReport, "a report's statistics were interleaved with another report");
            ++reports;
            index = next;
        }
        Require(reports == threads * reportsPerThread, "a pipeline statistics report is missing");
        std::printf("pipeline statistics: %zu reports of %zu statistics from %u threads, each in one piece\n", reports, perReport, threads);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pipeline statistics report test: %s\n", error.what());
        return 1;
    }
}
