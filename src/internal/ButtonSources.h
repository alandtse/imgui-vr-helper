// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
#pragma once

#include <cstdint>

namespace ImGuiVRHelper::ButtonSources
{
	enum class Source : uint8_t
	{
		Physical,
		Poke,
		Injected
	};

	class State
	{
	public:
		static constexpr uint32_t kKeyCount = 64;

		bool Set(Source source, uint32_t key, bool down)
		{
			if (key >= kKeyCount)
				return false;
			const uint64_t bit = uint64_t{ 1 } << key;
			auto& mask = Mask(source);
			if (down)
				mask |= bit;
			else
				mask &= ~bit;
			return true;
		}

		bool IsDown(uint32_t key) const
		{
			return key < kKeyCount && ((m_physical | m_poke | m_injected) & (uint64_t{ 1 } << key));
		}

		void Clear()
		{
			m_physical = 0;
			m_poke = 0;
			m_injected = 0;
		}

	private:
		uint64_t& Mask(Source source)
		{
			switch (source) {
			case Source::Physical:
				return m_physical;
			case Source::Poke:
				return m_poke;
			case Source::Injected:
				return m_injected;
			}
			return m_injected;
		}

		uint64_t m_physical = 0;
		uint64_t m_poke = 0;
		uint64_t m_injected = 0;
	};
}
