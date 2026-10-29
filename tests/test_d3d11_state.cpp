// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_test_macros.hpp>

#include "internal/D3D11StateGuard.h"

#include <d3dcompiler.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

using ImGuiVRHelper::Util::D3D11StateGuard;

namespace
{
	struct WarpDevice
	{
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;

		explicit WarpDevice(UINT flags = 0)
		{
			const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
			REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
				&level, 1, D3D11_SDK_VERSION, device.put(), nullptr, context.put())));
		}
	};

	// Getter references are owned, including shader class instances. Compare all
	// exposed pipeline binding slots, not just the representative slots we seed.
	struct Snapshot
	{
		std::vector<winrt::com_ptr<IUnknown>> objects;
		std::vector<unsigned char> values;

		template <class T>
		void Own(T* object)
		{
			winrt::com_ptr<IUnknown> owned;
			owned.attach(static_cast<IUnknown*>(object));
			objects.push_back(std::move(owned));
		}

		template <class T, size_t N>
		void Own(const std::array<T*, N>& array)
		{
			for (auto* object : array)
				Own(object);
		}

		template <class T>
		void Value(const T& value)
		{
			const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
			values.insert(values.end(), bytes, bytes + sizeof(value));
		}

		explicit Snapshot(ID3D11DeviceContext* context)
		{
			winrt::com_ptr<ID3D11DeviceContext1> context1;
			REQUIRE(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), context1.put_void())));
#define CAPTURE_STAGE(stage, type)                                                                                              \
	{                                                                                                                           \
		std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};                                 \
		std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views{};                            \
		std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};                                      \
		context->stage##GetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.data());                               \
		context->stage##GetShaderResources(0, static_cast<UINT>(views.size()), views.data());                                   \
		context->stage##GetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());                                    \
		Own(buffers);                                                                                                           \
		Own(views);                                                                                                             \
		Own(samplers);                                                                                                          \
		std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> first{}, sizes{};                                   \
		context1->stage##GetConstantBuffers1(0, static_cast<UINT>(buffers.size()), buffers.data(), first.data(), sizes.data()); \
		Own(buffers);                                                                                                           \
		Value(first);                                                                                                           \
		Value(sizes);                                                                                                           \
		type* shader = nullptr;                                                                                                 \
		std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};                                              \
		UINT count = static_cast<UINT>(instances.size());                                                                       \
		context->stage##GetShader(&shader, instances.data(), &count);                                                           \
		Own(shader);                                                                                                            \
		Value(count);                                                                                                           \
		for (UINT i = 0; i < count; ++i) Own(instances[i]);                                                                     \
	}
			CAPTURE_STAGE(VS, ID3D11VertexShader)
			CAPTURE_STAGE(HS, ID3D11HullShader)
			CAPTURE_STAGE(DS, ID3D11DomainShader)
			CAPTURE_STAGE(GS, ID3D11GeometryShader)
			CAPTURE_STAGE(PS, ID3D11PixelShader)
			CAPTURE_STAGE(CS, ID3D11ComputeShader)
#undef CAPTURE_STAGE
			std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
			context->CSGetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data());
			Own(uavs);
			context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
				static_cast<UINT>(uavs.size()), uavs.data());
			Own(uavs);
			std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
			ID3D11DepthStencilView* depthView = nullptr;
			context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depthView);
			Own(targets);
			Own(depthView);

			ID3D11BlendState* blend = nullptr;
			FLOAT blendFactor[4]{};
			UINT sampleMask = 0;
			context->OMGetBlendState(&blend, blendFactor, &sampleMask);
			Own(blend);
			Value(blendFactor);
			Value(sampleMask);
			ID3D11DepthStencilState* depth = nullptr;
			UINT stencilRef = 0;
			context->OMGetDepthStencilState(&depth, &stencilRef);
			Own(depth);
			Value(stencilRef);

			ID3D11RasterizerState* rasterizer = nullptr;
			context->RSGetState(&rasterizer);
			Own(rasterizer);
			std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
			UINT count = static_cast<UINT>(viewports.size());
			context->RSGetViewports(&count, viewports.data());
			Value(count);
			for (UINT i = 0; i < count; ++i) Value(viewports[i]);
			std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> rects{};
			count = static_cast<UINT>(rects.size());
			context->RSGetScissorRects(&count, rects.data());
			Value(count);
			for (UINT i = 0; i < count; ++i) Value(rects[i]);

			std::array<ID3D11Buffer*, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices{};
			std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides{}, offsets{};
			context->IAGetVertexBuffers(0, static_cast<UINT>(vertices.size()), vertices.data(), strides.data(), offsets.data());
			Own(vertices);
			Value(strides);
			Value(offsets);
			ID3D11Buffer* indices = nullptr;
			DXGI_FORMAT format{};
			UINT offset = 0;
			context->IAGetIndexBuffer(&indices, &format, &offset);
			Own(indices);
			Value(format);
			Value(offset);
			ID3D11InputLayout* layout = nullptr;
			context->IAGetInputLayout(&layout);
			Own(layout);
			D3D11_PRIMITIVE_TOPOLOGY topology{};
			context->IAGetPrimitiveTopology(&topology);
			Value(topology);
			std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> streamOut{};
			context->SOGetTargets(static_cast<UINT>(streamOut.size()), streamOut.data());
			Own(streamOut);
			ID3D11Predicate* predicate = nullptr;
			BOOL predicateValue = FALSE;
			context->GetPredication(&predicate, &predicateValue);
			Own(predicate);
			Value(predicateValue);
		}

		void RequireEqual(const Snapshot& other) const
		{
			REQUIRE(values == other.values);
			REQUIRE(objects.size() == other.objects.size());
			for (size_t i = 0; i < objects.size(); ++i) {
				INFO("pipeline object index " << i);
				REQUIRE(objects[i].get() == other.objects[i].get());
			}
		}
	};

	// Resolve the SDK compiler dynamically: tests only need to link d3d11.lib.
	// Keep it loaded until all bytecode blobs have been released.
	struct Compiler
	{
		HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
		Compiler() { REQUIRE(module != nullptr); }
		~Compiler()
		{
			if (module)
				FreeLibrary(module);
		}
		Compiler(const Compiler&) = delete;
		Compiler& operator=(const Compiler&) = delete;

		winrt::com_ptr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile)
		{
			const auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"));
			REQUIRE(compile != nullptr);
			winrt::com_ptr<ID3DBlob> code, errors;
			const auto result = compile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile,
				D3DCOMPILE_ENABLE_STRICTNESS, 0, code.put(), errors.put());
			INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no shader diagnostics"));
			REQUIRE(SUCCEEDED(result));
			return code;
		}
	};

	struct Texture
	{
		winrt::com_ptr<ID3D11Texture2D> resource;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;

		explicit Texture(ID3D11Device* device)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = 16;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
			REQUIRE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, resource.put())));
			REQUIRE(SUCCEEDED(device->CreateShaderResourceView(resource.get(), nullptr, srv.put())));
			REQUIRE(SUCCEEDED(device->CreateUnorderedAccessView(resource.get(), nullptr, uav.put())));
			REQUIRE(SUCCEEDED(device->CreateRenderTargetView(resource.get(), nullptr, rtv.put())));
		}
	};

	winrt::com_ptr<ID3D11Buffer> Buffer(ID3D11Device* device, UINT flags)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 1024;
		desc.BindFlags = flags;
		winrt::com_ptr<ID3D11Buffer> buffer;
		REQUIRE(SUCCEEDED(device->CreateBuffer(&desc, nullptr, buffer.put())));
		return buffer;
	}

	void BindReadViews(ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv)
	{
		context->VSSetShaderResources(127, 1, &srv);
		context->HSSetShaderResources(127, 1, &srv);
		context->DSSetShaderResources(127, 1, &srv);
		context->GSSetShaderResources(127, 1, &srv);
		context->PSSetShaderResources(127, 1, &srv);
		context->CSSetShaderResources(127, 1, &srv);
	}

	void BindRepresentativePipeline(WarpDevice& warp, Compiler& compiler)
	{
		auto* device = warp.device.get();
		auto* context = warp.context.get();
		const char* graphics = R"(
struct V { float4 position : SV_Position; };
V VS(float4 position : POSITION) { V v; v.position = position; return v; }
[maxvertexcount(3)] void GS(triangle V input[3], inout TriangleStream<V> output) {
    output.Append(input[0]); output.Append(input[1]); output.Append(input[2]);
}
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors Patch(InputPatch<V, 3> input) {
    Factors f; f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("Patch")]
V HS(InputPatch<V, 3> input, uint id : SV_OutputControlPointID) { return input[id]; }
[domain("tri")] V DS(Factors f, float3 uv : SV_DomainLocation, const OutputPatch<V, 3> input) {
    V v; v.position = uv.x * input[0].position + uv.y * input[1].position + uv.z * input[2].position; return v;
}
)";
		auto vsCode = compiler.Compile(graphics, "VS", "vs_5_0");
		auto gsCode = compiler.Compile(graphics, "GS", "gs_5_0");
		auto hsCode = compiler.Compile(graphics, "HS", "hs_5_0");
		auto dsCode = compiler.Compile(graphics, "DS", "ds_5_0");
		auto psCode = compiler.Compile(R"(
interface IColor { float4 Shade(); };
class SolidColor : IColor { float4 color; float4 Shade() { return color; } };
SolidColor solid;
IColor selected;
float4 PS() : SV_Target { return selected.Shade() + solid.color * 0.001; }
)",
			"PS", "ps_5_0");
		auto csCode = compiler.Compile("RWTexture2D<float4> target : register(u7); [numthreads(1,1,1)] void CS() { target[uint2(0,0)] = float4(1,0,0,1); }", "CS", "cs_5_0");
		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		winrt::com_ptr<ID3D11GeometryShader> gs;
		winrt::com_ptr<ID3D11PixelShader> ps;
		winrt::com_ptr<ID3D11ComputeShader> cs;
		winrt::com_ptr<ID3D11ClassLinkage> linkage;
		winrt::com_ptr<ID3D11ClassInstance> instance;
		REQUIRE(SUCCEEDED(device->CreateClassLinkage(linkage.put())));
		REQUIRE(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.put())));
		REQUIRE(SUCCEEDED(device->CreateHullShader(hsCode->GetBufferPointer(), hsCode->GetBufferSize(), nullptr, hs.put())));
		REQUIRE(SUCCEEDED(device->CreateDomainShader(dsCode->GetBufferPointer(), dsCode->GetBufferSize(), nullptr, ds.put())));
		REQUIRE(SUCCEEDED(device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, gs.put())));
		REQUIRE(SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), linkage.get(), ps.put())));
		REQUIRE(SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, cs.put())));
		REQUIRE(SUCCEEDED(linkage->GetClassInstance("solid", 0, instance.put())));
		auto* classInstance = instance.get();
		context->VSSetShader(vs.get(), nullptr, 0);
		context->HSSetShader(hs.get(), nullptr, 0);
		context->DSSetShader(ds.get(), nullptr, 0);
		context->GSSetShader(gs.get(), nullptr, 0);
		context->PSSetShader(ps.get(), &classInstance, 1);
		context->CSSetShader(cs.get(), nullptr, 0);

		D3D11_INPUT_ELEMENT_DESC element{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
		winrt::com_ptr<ID3D11InputLayout> layout;
		REQUIRE(SUCCEEDED(device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), layout.put())));
		context->IASetInputLayout(layout.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
		auto vertices = Buffer(device, D3D11_BIND_VERTEX_BUFFER);
		auto indices = Buffer(device, D3D11_BIND_INDEX_BUFFER);
		auto streamOut = Buffer(device, D3D11_BIND_STREAM_OUTPUT);
		auto constants = Buffer(device, D3D11_BIND_CONSTANT_BUFFER);
		auto* vb = vertices.get();
		const UINT stride = 16, offset = 32;
		context->IASetVertexBuffers(31, 1, &vb, &stride, &offset);
		context->IASetIndexBuffer(indices.get(), DXGI_FORMAT_R16_UINT, 6);
		auto* so = streamOut.get();
		context->SOSetTargets(1, &so, &offset);
		auto* cb = constants.get();
		context->VSSetConstantBuffers(13, 1, &cb);
		context->HSSetConstantBuffers(13, 1, &cb);
		context->DSSetConstantBuffers(13, 1, &cb);
		context->GSSetConstantBuffers(13, 1, &cb);
		context->PSSetConstantBuffers(13, 1, &cb);
		context->CSSetConstantBuffers(13, 1, &cb);
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
			options.ConstantBufferOffsetting) {
			winrt::com_ptr<ID3D11DeviceContext1> context1;
			REQUIRE(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), context1.put_void())));
			const UINT first = 16, size = 16;
			context1->VSSetConstantBuffers1(13, 1, &cb, &first, &size);
			context1->HSSetConstantBuffers1(13, 1, &cb, &first, &size);
			context1->DSSetConstantBuffers1(13, 1, &cb, &first, &size);
			context1->GSSetConstantBuffers1(13, 1, &cb, &first, &size);
			context1->PSSetConstantBuffers1(13, 1, &cb, &first, &size);
			context1->CSSetConstantBuffers1(13, 1, &cb, &first, &size);
		}
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		REQUIRE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.put())));
		auto* ss = sampler.get();
		context->VSSetSamplers(15, 1, &ss);
		context->HSSetSamplers(15, 1, &ss);
		context->DSSetSamplers(15, 1, &ss);
		context->GSSetSamplers(15, 1, &ss);
		context->PSSetSamplers(15, 1, &ss);
		context->CSSetSamplers(15, 1, &ss);
		Texture read(device), compute(device), output(device), pixelWrite(device);
		BindReadViews(context, read.srv.get());
		auto* uav = compute.uav.get();
		context->CSSetUnorderedAccessViews(7, 1, &uav, nullptr);
		auto* rtv = output.rtv.get();
		auto* pixelUav = pixelWrite.uav.get();
		D3D11_TEXTURE2D_DESC depthDesc{};
		depthDesc.Width = depthDesc.Height = 16;
		depthDesc.MipLevels = depthDesc.ArraySize = depthDesc.SampleDesc.Count = 1;
		depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		winrt::com_ptr<ID3D11Texture2D> depthTexture;
		winrt::com_ptr<ID3D11DepthStencilView> depthView;
		REQUIRE(SUCCEEDED(device->CreateTexture2D(&depthDesc, nullptr, depthTexture.put())));
		REQUIRE(SUCCEEDED(device->CreateDepthStencilView(depthTexture.get(), nullptr, depthView.put())));
		context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, depthView.get(), 3, 1, &pixelUav, nullptr);

		D3D11_BLEND_DESC blendDesc{};
		for (auto& target : blendDesc.RenderTarget) {
			target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
			target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		}
		winrt::com_ptr<ID3D11BlendState> blend;
		REQUIRE(SUCCEEDED(device->CreateBlendState(&blendDesc, blend.put())));
		const FLOAT factor[4]{ 0.1f, 0.2f, 0.3f, 0.4f };
		context->OMSetBlendState(blend.get(), factor, 0x12345678);
		D3D11_DEPTH_STENCIL_DESC depthStateDesc{};
		depthStateDesc.DepthEnable = TRUE;
		depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStateDesc.DepthFunc = D3D11_COMPARISON_GREATER;
		depthStateDesc.StencilReadMask = depthStateDesc.StencilWriteMask = 0xff;
		depthStateDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depthStateDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		depthStateDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depthStateDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		depthStateDesc.BackFace = depthStateDesc.FrontFace;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		REQUIRE(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc, depthState.put())));
		context->OMSetDepthStencilState(depthState.get(), 37);
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_WIREFRAME;
		rasterDesc.CullMode = D3D11_CULL_FRONT;
		rasterDesc.ScissorEnable = TRUE;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		REQUIRE(SUCCEEDED(device->CreateRasterizerState(&rasterDesc, raster.put())));
		context->RSSetState(raster.get());
		const D3D11_VIEWPORT viewport{ 1, 2, 8, 9, 0.25f, 0.75f };
		const D3D11_RECT scissor{ 1, 2, 7, 8 };
		context->RSSetViewports(1, &viewport);
		context->RSSetScissorRects(1, &scissor);
		D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
		winrt::com_ptr<ID3D11Predicate> predicate;
		REQUIRE(SUCCEEDED(device->CreatePredicate(&queryDesc, predicate.put())));
		context->SetPredication(predicate.get(), TRUE);
		// Context bindings now own these objects after the local references die.
	}
}

TEST_CASE("D3D11 guard restores graphics, compute and shader class instances", "[d3d11][warp]")
{
	WarpDevice warp;
	Compiler compiler;
	const Snapshot defaults(warp.context.get());
	BindRepresentativePipeline(warp, compiler);
	const Snapshot caller(warp.context.get());
	{
		D3D11StateGuard guard(warp.context.get());
		REQUIRE(guard.IsValid());
		defaults.RequireEqual(Snapshot(warp.context.get()));
		// Populate a completely different isolated pipeline, not merely ClearState.
		BindRepresentativePipeline(warp, compiler);
	}
	caller.RequireEqual(Snapshot(warp.context.get()));
	// A second lease must start clean, not with the preceding overlay's state.
	{
		D3D11StateGuard guard(warp.context.get());
		REQUIRE(guard.IsValid());
		defaults.RequireEqual(Snapshot(warp.context.get()));
	}
	caller.RequireEqual(Snapshot(warp.context.get()));
}

TEST_CASE("D3D11 guard isolates automatic cross-stage resource unbinding", "[d3d11][warp]")
{
	WarpDevice warp;
	Texture texture(warp.device.get());
	BindReadViews(warp.context.get(), texture.srv.get());
	const Snapshot caller(warp.context.get());
	{
		D3D11StateGuard guard(warp.context.get());
		REQUIRE(guard.IsValid());
		BindReadViews(warp.context.get(), texture.srv.get());
		auto* uav = texture.uav.get();
		warp.context->CSSetUnorderedAccessViews(7, 1, &uav, nullptr);
#define REQUIRE_UNBOUND(stage)                                       \
	do {                                                             \
		winrt::com_ptr<ID3D11ShaderResourceView> view;               \
		warp.context->stage##GetShaderResources(127, 1, view.put()); \
		REQUIRE(view == nullptr);                                    \
	} while (false)
		REQUIRE_UNBOUND(VS);
		REQUIRE_UNBOUND(HS);
		REQUIRE_UNBOUND(DS);
		REQUIRE_UNBOUND(GS);
		REQUIRE_UNBOUND(PS);
		REQUIRE_UNBOUND(CS);
#undef REQUIRE_UNBOUND
		winrt::com_ptr<ID3D11UnorderedAccessView> bound;
		warp.context->CSGetUnorderedAccessViews(7, 1, bound.put());
		REQUIRE(bound.get() == texture.uav.get());
		// The reverse conflict must not leak either: an output blocks SRV binding.
		auto* srv = texture.srv.get();
		warp.context->PSSetShaderResources(127, 1, &srv);
		winrt::com_ptr<ID3D11ShaderResourceView> blocked;
		warp.context->PSGetShaderResources(127, 1, blocked.put());
		REQUIRE(blocked == nullptr);
	}
	caller.RequireEqual(Snapshot(warp.context.get()));

	warp.context->ClearState();
	auto* uav = texture.uav.get();
	warp.context->CSSetUnorderedAccessViews(7, 1, &uav, nullptr);
	const Snapshot outputCaller(warp.context.get());
	{
		D3D11StateGuard guard(warp.context.get());
		REQUIRE(guard.IsValid());
		BindReadViews(warp.context.get(), texture.srv.get());
		auto* rtv = texture.rtv.get();
		warp.context->OMSetRenderTargets(1, &rtv, nullptr);
	}
	outputCaller.RequireEqual(Snapshot(warp.context.get()));
}

TEST_CASE("D3D11 guard restores null state during exception unwinding and nesting", "[d3d11][warp]")
{
	WarpDevice warp(D3D11_CREATE_DEVICE_SINGLETHREADED);
	Compiler compiler;
	const Snapshot defaults(warp.context.get());
	REQUIRE_THROWS_AS(([&] {
		D3D11StateGuard outer(warp.context.get());
		REQUIRE(outer.IsValid());
		BindRepresentativePipeline(warp, compiler);
		const Snapshot outerState(warp.context.get());
		{
			D3D11StateGuard inner(warp.context.get());
			REQUIRE(inner.IsValid());
			defaults.RequireEqual(Snapshot(warp.context.get()));
			warp.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
		}
		outerState.RequireEqual(Snapshot(warp.context.get()));
		throw std::runtime_error("overlay failure");
	}()),
		std::runtime_error);
	defaults.RequireEqual(Snapshot(warp.context.get()));
}

TEST_CASE("D3D11 guard rejects null and deferred contexts without changing them", "[d3d11][warp]")
{
	D3D11StateGuard missing(nullptr);
	REQUIRE_FALSE(missing.IsValid());
	WarpDevice warp;
	winrt::com_ptr<ID3D11DeviceContext> deferred;
	REQUIRE(SUCCEEDED(warp.device->CreateDeferredContext(0, deferred.put())));
	deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const Snapshot immediateBefore(warp.context.get());
	{
		D3D11StateGuard unsupported(deferred.get());
		REQUIRE_FALSE(unsupported.IsValid());
	}
	immediateBefore.RequireEqual(Snapshot(warp.context.get()));
	// Deferred contexts do not support pipeline getters. The rejected context
	// remains usable for recording and executing its command list.
	winrt::com_ptr<ID3D11CommandList> commands;
	REQUIRE(SUCCEEDED(deferred->FinishCommandList(FALSE, commands.put())));
	warp.context->ExecuteCommandList(commands.get(), TRUE);
	immediateBefore.RequireEqual(Snapshot(warp.context.get()));
}

TEST_CASE("D3D11 clean state cache follows device identity", "[d3d11][warp]")
{
	WarpDevice first, second;
	first.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	second.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
	const Snapshot firstBefore(first.context.get()), secondBefore(second.context.get());
	{
		D3D11StateGuard guard(first.context.get());
		REQUIRE(guard.IsValid());
		{
			D3D11StateGuard other(second.context.get());
			REQUIRE(other.IsValid());
		}
	}
	firstBefore.RequireEqual(Snapshot(first.context.get()));
	secondBefore.RequireEqual(Snapshot(second.context.get()));
	{
		D3D11StateGuard other(second.context.get());
		REQUIRE(other.IsValid());
	}
	secondBefore.RequireEqual(Snapshot(second.context.get()));
}
