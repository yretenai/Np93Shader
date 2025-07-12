// SPDX-FileCopyrightText: 2025 Np93-237
//
// SPDX-License-Identifier: EUPL-1.2

#include <glslang/MachineIndependent/Versions.h>   // for ENoProfile
#include <glslang/Public/ResourceLimits.h>         // for GetDefaultResources
#include <glslang/Public/ShaderLang.h>             // for TShader, TProgram
#include <glslang/SPIRV/GlslangToSpv.h>            // for SpvOptions, Glslan...

#include <spirv_cross/spirv_cross_containers.hpp>  // for SmallVector, Vecto...
#include <spirv_cross/spirv_cross.hpp>             // for Compiler, EntryPoint
#include <spirv_cross/spirv_glsl.hpp>              // for CompilerGLSL

#include <vulkan/utility/vk_dispatch_table.h>      // for VkuDeviceDispatchT...
#include <vulkan/vk_layer.h>                       // for VkLayerDeviceCreat...
#include <vulkan/vk_platform.h>                    // for VKAPI_CALL
#include <vulkan/vulkan_core.h>                    // for VkResult, PFN_vkVo...

#include <array>                                   // for array
#include <cstdint>                                 // for uint32_t, uint64_t
#include <cstdlib>                                 // for getenv
#include <filesystem>                              // for path, operator/
#include <format>                                  // for format
#include <fstream>                                 // for basic_ostream, cha...
#include <iostream>                                // for cout
#include <map>                                     // for map, allocator
#include <mutex>                                   // for mutex, lock_guard
#include <optional>                                // for optional, nullopt
#include <sstream>                                 // for basic_stringstream
#include <string.h>                                // for strcmp, strlen
#include <string>                                  // for basic_string, string
#include <utility>                                 // for pair
#include <vector>                                  // for vector

#include "hash.h"                                  // for CreateHash

#if defined(_WIN32)
#define VK_LAYER_EXPORT extern "C" dllexport()
#else
#define VK_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define GETPROCADDR(func) \
	if (!strcmp(pName, "vk" #func)) \
		return (PFN_vkVoidFunction) & Np93_##func;

std::ofstream out;

bool CheckEnvBool(const char *name) {
	auto var = getenv(name);
	if (var == nullptr) {
		return false;
	}

	if (strlen(var) > 0 && var[0] == '0') {
		return false;
	}

	return true;
}

struct GlobalOptions {
	std::filesystem::path shader_output;
	bool dump_shaders;
	bool recompile_shaders;

	GlobalOptions() {
		auto path = getenv("NP93_SHADER_PATH");
		shader_output = path == nullptr
												? (std::filesystem::current_path() / ".np93")
												: std::filesystem::path(path);
		std::filesystem::create_directories(shader_output);
		out.open(shader_output / "np.log", std::ofstream::out);
		std::cout << "writing to " << shader_output << std::endl;
		out << "writing to " << shader_output << std::endl;

		dump_shaders = CheckEnvBool("NP93_SHADER_DUMP");
		recompile_shaders = CheckEnvBool("NP93_SHADER_RECOMPILE");
		glslang::InitializeProcess();
	}

	~GlobalOptions() {
		glslang::FinalizeProcess();
		out.flush();
		out.close();
	}
};

const GlobalOptions global;

std::mutex dispatch_lock;
std::mutex write_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

std::map<VkInstance, VkuInstanceDispatchTable> instance_dispatch;
std::map<VkDevice, VkuDeviceDispatchTable> device_dispatch;

std::array<std::string, EShLangMesh + 1> shader_to_suffix{
		".vsh",       ".control", ".eval",  ".gsh",  ".fsh",  ".csh",  ".ray",
		".intersect", ".hit",     ".close", ".miss", ".call", ".task", ".msh",
};

VkLayerInstanceCreateInfo *GetLinkInfo(VkLayerInstanceCreateInfo *link) {
	auto current = link;
	while (current &&
				 (current->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
					current->function != VK_LAYER_LINK_INFO)) {
		current = (VkLayerInstanceCreateInfo *)
									current->pNext; // raw cast because it's a const,
	}
	return current;
}

VkLayerDeviceCreateInfo *GetLinkInfo(VkLayerDeviceCreateInfo *link) {
	auto current = link;
	while (current &&
				 (current->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
					current->function != VK_LAYER_LINK_INFO)) {
		current = (VkLayerDeviceCreateInfo *)
									current->pNext; // raw cast because it's a const,
	}
	return current;
}

std::string DisassembleSPIRV(const VkShaderModuleCreateInfo *pCreateInfo) {
	if(pCreateInfo->pCode == nullptr || (pCreateInfo->codeSize % 4) > 0) {
		return {};
	}

	try {
		spirv_cross::CompilerGLSL compiler(pCreateInfo->pCode, pCreateInfo->codeSize >> 2);
		spirv_cross::CompilerGLSL::Options options;
		options.version = 450;
		options.es = false;
		options.vulkan_semantics = true;
		compiler.set_common_options(options);
		return compiler.compile();
	} catch(...) {
		return {};
	}
}

std::optional<EShLanguage>
DetermineSPIRVShaderType(const VkShaderModuleCreateInfo *pCreateInfo) {
	try {
		spirv_cross::Compiler compiler(pCreateInfo->pCode, pCreateInfo->codeSize >> 2);
		auto shaders = compiler.get_entry_points_and_stages();
		if (shaders.empty()) {
			out << "no shader programs?" << std::endl;
			return std::nullopt;
		}

		if (shaders.size() > 1) {
			out << "> 1 shader programs?" << std::endl;
			return std::nullopt;
		}

		switch (shaders[0].execution_model) {
		case spv::ExecutionModelVertex:
			return EShLangVertex;
		case spv::ExecutionModelFragment:
			return EShLangFragment;
		default:
			out << "can't determine program type " << shaders[0].execution_model << std::endl;
			return std::nullopt;
		}
	} catch(...) { return std::nullopt; }
}

std::vector<uint32_t> CompileGLSL(std::string &glsl, EShLanguage lang) {
	const auto MESSAGES = static_cast<EShMessages>(EShMsgDefault | EShMsgVulkanRules | EShMsgSpvRules);

	std::vector<uint32_t> new_spirv;

	auto resources = GetDefaultResources();
	glslang::TShader shader(lang);
	auto cstr = glsl.c_str();
	const auto length = static_cast<int>(glsl.length());
	shader.setStringsWithLengths(&cstr, &length, 1);

	shader.setEntryPoint("main");
	shader.setSourceEntryPoint("main");
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_4);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

	static auto includer = glslang::TShader::ForbidIncluder();
	std::string preprocessed;
	if (!shader.preprocess(resources, 450, ENoProfile, false, false, MESSAGES, &preprocessed, includer)) {
		out << "Failed to preprocess GLSL:\n" << shader.getInfoLog() << "\n" << shader.getInfoLog() << std::endl;
		return new_spirv;
	}

	if (!shader.parse(resources, 450, true, MESSAGES)) {
		out << "Failed to parse GLSL:\n" << shader.getInfoLog() << "\n" << shader.getInfoLog() << std::endl;
		return new_spirv;
	}

	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(MESSAGES)) {
		out << "Failed to link shader prorgam:\n" << program.getInfoLog() << "\n" << program.getInfoDebugLog() << std::endl;
		return new_spirv;
	}

	glslang::SpvOptions options;
	options.generateDebugInfo = true;
	options.disableOptimizer = false;
	options.validate = false;
	options.optimizeSize = true;
	options.disassemble = false;
	glslang::GlslangToSpv(*program.getIntermediate(lang), new_spirv, nullptr,
												&options);
	return new_spirv;
}

VkShaderModuleCreateInfo CreateShaderModuleCore(const VkShaderModuleCreateInfo* pCreateInfo, const VkShaderStageFlagBits test_bits) {
	try {
		if(pCreateInfo->pNext != nullptr) {
			return {};
		}

		const auto hash = CreateHash(pCreateInfo->pCode, static_cast<int>(pCreateInfo->codeSize));
		auto type_opt = DetermineSPIRVShaderType(pCreateInfo);

		if (type_opt.has_value()) {
			const auto type = type_opt.value();
			if (test_bits) {
				if(type == EShLangFragment && (test_bits & ~VK_SHADER_STAGE_FRAGMENT_BIT) != 0) {
					out << "shader bits mismatch... expected a match for " << type << " got " << test_bits << std::endl;
					return {};
				} else if(type == EShLangVertex && (test_bits & ~VK_SHADER_STAGE_VERTEX_BIT) != 0) {
					out << "shader bits mismatch... expected a match for " << type << " got " << test_bits << std::endl;
					return {};
				}
			}

			const auto path = global.shader_output / std::format("{:x}{}", hash, shader_to_suffix[type]);
			if (global.dump_shaders) {
				scoped_lock lock(write_lock);
				if(std::filesystem::exists(path)) {
					return {};
				}

				out << "saving " << path.filename().string() << std::endl;
				auto shader = DisassembleSPIRV(pCreateInfo);
				std::ofstream stream;
				stream.open(path, std::ofstream::out);
				stream << shader << std::flush;
				return {};
			}

			if (std::filesystem::exists(path)) {
				std::ifstream stream;
				stream.open(path, std::ifstream::in);
				std::stringstream buffer;
				buffer << stream.rdbuf();

				auto code = buffer.str();
				if (code.empty()) {
					return {};
				}

				auto new_spirv = CompileGLSL(code, type);
				if (new_spirv.empty()) {
					out << "couldn't compile shader " << path.filename().string() << std::endl;
					return {};
				}

				VkShaderModuleCreateInfo createInfo = {};
				createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
				createInfo.pCode = new_spirv.data();
				createInfo.codeSize = new_spirv.size() * 4;
				out << "loaded modified shader " << path.filename().string() << std::endl;
				return createInfo;
			}
		}
	} catch(...) { }
	return {};
}

// dxvk has 5 stages, and 1 pipeline per call, expand as needed.
VK_LAYER_EXPORT VkResult Np93_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
	if(pCreateInfos == nullptr || createInfoCount != 1 || pCreateInfos->stageCount > 5 || pCreateInfos->stageCount == 0 || pCreateInfos->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
		{
			scoped_lock lock(dispatch_lock);
			return device_dispatch[device].CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
		}
	}

	std::vector<void*> free_track;

	// manual shallow copy because memcpy somehow just dies.
	VkGraphicsPipelineCreateInfo pipelineCreateInfo;
	pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCreateInfo.pNext = pCreateInfos->pNext;
	pipelineCreateInfo.flags = pCreateInfos->flags;
	pipelineCreateInfo.stageCount = pCreateInfos->stageCount;
	pipelineCreateInfo.pStages = pCreateInfos->pStages;
	pipelineCreateInfo.pVertexInputState = pCreateInfos->pVertexInputState;
	pipelineCreateInfo.pInputAssemblyState = pCreateInfos->pInputAssemblyState;
	pipelineCreateInfo.pTessellationState = pCreateInfos->pTessellationState;
	pipelineCreateInfo.pViewportState = pCreateInfos->pViewportState;
	pipelineCreateInfo.pRasterizationState = pCreateInfos->pRasterizationState;
	pipelineCreateInfo.pMultisampleState = pCreateInfos->pMultisampleState;
	pipelineCreateInfo.pDepthStencilState = pCreateInfos->pDepthStencilState;
	pipelineCreateInfo.pColorBlendState = pCreateInfos->pColorBlendState;
	pipelineCreateInfo.pDynamicState = pCreateInfos->pDynamicState;
	pipelineCreateInfo.layout = pCreateInfos->layout;
	pipelineCreateInfo.renderPass = pCreateInfos->renderPass;
	pipelineCreateInfo.subpass = pCreateInfos->subpass;
	pipelineCreateInfo.basePipelineHandle = pCreateInfos->basePipelineHandle;
	pipelineCreateInfo.basePipelineIndex = pCreateInfos->basePipelineIndex;

	std::array<VkPipelineShaderStageCreateInfo, 5> stages;
	std::array<VkShaderModuleCreateInfo, 5> shaders;

	bool modified_at_all = false;
	for(uint32_t stageIndex = 0; stageIndex < pipelineCreateInfo.stageCount; ++stageIndex) {
		const auto stageInfo = &pipelineCreateInfo.pStages[stageIndex];

		if(stageInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO) {
			continue;
		}

		stages[stageIndex].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[stageIndex].pNext = stageInfo->pNext;
		stages[stageIndex].flags = stageInfo->flags;
		stages[stageIndex].stage = stageInfo->stage;
		stages[stageIndex].module = stageInfo->module;
		stages[stageIndex].pName = stageInfo->pName;
		stages[stageIndex].pSpecializationInfo = stageInfo->pSpecializationInfo;

		if(stages[stageIndex].pNext == nullptr) {
			continue;
		}

		if(reinterpret_cast<const VkBaseOutStructure *>(stages[stageIndex].pNext)->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
			continue;
		}

		shaders[stageIndex] = CreateShaderModuleCore(reinterpret_cast<const VkShaderModuleCreateInfo *>(stages[stageIndex].pNext), static_cast<VkShaderStageFlagBits>(stages[stageIndex].stage & VK_SHADER_STAGE_ALL_GRAPHICS));
		if(shaders[stageIndex].sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
			stages[stageIndex].pNext = &shaders[stageIndex];
			modified_at_all = true;
		}
	}
	pipelineCreateInfo.pStages = stages.data();

	{
		scoped_lock lock(dispatch_lock);
		return device_dispatch[device].CreateGraphicsPipelines(device, pipelineCache, createInfoCount, modified_at_all ? &pipelineCreateInfo : pCreateInfos, pAllocator, pPipelines);
	}
}

VK_LAYER_EXPORT VkResult Np93_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
	if (pCreateInfo == nullptr || pCreateInfo->pNext != nullptr || pCreateInfo->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
		out << "Intercepted vkCreateShaderModule is invalid!" << std::endl;
		{
			scoped_lock lock(dispatch_lock);
			return device_dispatch[device].CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
		}
	}

	auto newCreateInfo = CreateShaderModuleCore(pCreateInfo, static_cast<VkShaderStageFlagBits>(0));
	if(newCreateInfo.sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
		scoped_lock lock(dispatch_lock);
		return device_dispatch[device].CreateShaderModule(device, &newCreateInfo, pAllocator, pShaderModule);
	}

	out << "falling back to normal shader" << std::endl;
	{
		scoped_lock lock(dispatch_lock);
		return device_dispatch[device].CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
	}
}

VK_LAYER_EXPORT VkResult Np93_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
	auto layerCreateInfo = GetLinkInfo((VkLayerInstanceCreateInfo *)pCreateInfo->pNext);
	if (layerCreateInfo == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	if (gpa == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	auto createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");
	if (createFunc == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto result = createFunc(pCreateInfo, pAllocator, pInstance);
	if (result != VK_SUCCESS) {
		return result;
	}

	VkuInstanceDispatchTable dispatchTable;
	vkuInitInstanceDispatchTable(*pInstance, &dispatchTable, gpa);
	{
		scoped_lock lock(dispatch_lock);
		instance_dispatch[*pInstance] = dispatchTable;
	}

	return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL Np93_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
	auto layerCreateInfo =
			GetLinkInfo((VkLayerDeviceCreateInfo *)pCreateInfo->pNext);
	if (layerCreateInfo == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	if (gipa == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	if (gdpa == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

	auto createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
	if (createFunc == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	auto result = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (result != VK_SUCCESS) {
		return result;
	}

	VkuDeviceDispatchTable dispatchTable;
	vkuInitDeviceDispatchTable(*pDevice, &dispatchTable, gdpa);
	{
		scoped_lock lock(dispatch_lock);
		device_dispatch[*pDevice] = dispatchTable;
	}

	return VK_SUCCESS;
}

VK_LAYER_EXPORT void Np93_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *) {
	scoped_lock lock(dispatch_lock);
	instance_dispatch.erase(instance);
}

VK_LAYER_EXPORT void Np93_DestroyDevice(VkDevice device, const VkAllocationCallbacks *) {
	scoped_lock lock(dispatch_lock);
	device_dispatch.erase(device);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
Np93_GetInstanceProcAddr(VkInstance instance, const char *pName) {
	GETPROCADDR(GetInstanceProcAddr);
	GETPROCADDR(CreateInstance);
	GETPROCADDR(DestroyInstance);
	GETPROCADDR(CreateDevice);
	GETPROCADDR(DestroyDevice);

	{
		scoped_lock lock(dispatch_lock);
		return instance_dispatch[instance].GetInstanceProcAddr(instance, pName);
	}
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL
Np93_GetDeviceProcAddr(VkDevice device, const char *pName) {
	GETPROCADDR(GetDeviceProcAddr);
	GETPROCADDR(CreateShaderModule);
	GETPROCADDR(CreateGraphicsPipelines);

	{
		scoped_lock lock(dispatch_lock);
		return device_dispatch[device].GetDeviceProcAddr(device, pName);
	}
}
