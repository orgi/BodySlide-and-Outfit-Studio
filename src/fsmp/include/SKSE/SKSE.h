// Stand-in for CommonLibSSE's SKSE/SKSE.h. The previewer is no SKSE plugin:
// the interfaces only exist so FSMP's sources compile, and calling them is an
// error (see StandIns.cpp).
#pragma once

#include <RE/Skyrim.h>
#include <SKSE/Logger.h>

#include <cstddef>
#include <cstdint>

namespace SKSE
{
	using PluginHandle = std::uint32_t;

	struct CameraEvent
	{
		RE::TESCameraState* oldState{ nullptr };
		RE::TESCameraState* newState{ nullptr };
	};

	class MessagingInterface
	{
	public:
		enum : std::uint32_t
		{
			kPostLoad,
			kPostPostLoad,
			kPreLoadGame,
			kPostLoadGame,
			kSaveGame,
			kDeleteGame,
			kInputLoaded,
			kNewGame,
			kDataLoaded
		};

		bool Dispatch(std::uint32_t a_messageType, void* a_data, std::uint32_t a_dataLen, const char* a_receiver) const;
	};

	class LoadInterface
	{
	public:
		enum : std::uint32_t
		{
			kInvalid = 0,
			kScaleform,
			kPapyrus,
			kSerialization,
			kTask,
			kMessaging,
			kObject,
			kTrampoline,
			kTotal
		};

		[[nodiscard]] PluginHandle GetPluginHandle() const;
		[[nodiscard]] void* QueryInterface(std::uint32_t a_id) const;
	};

	class SerializationInterface
	{
	public:
		bool OpenRecord(std::uint32_t a_type, std::uint32_t a_version) const;
		bool WriteRecordData(const void* a_buf, std::uint32_t a_length) const;
		bool GetNextRecordInfo(std::uint32_t& a_type, std::uint32_t& a_version, std::uint32_t& a_length) const;
		std::uint32_t ReadRecordData(void* a_buf, std::uint32_t a_length) const;
	};

	class Trampoline
	{
	public:
		template <std::size_t N, class F>
		std::uintptr_t write_call(std::uintptr_t a_src, F)
		{
			return Unavailable(a_src);
		}

		template <std::size_t N, class F>
		std::uintptr_t write_branch(std::uintptr_t a_src, F)
		{
			return Unavailable(a_src);
		}

		template <class T>
		void* allocate(T&)
		{
			Unavailable(0);
			return nullptr;
		}

	private:
		static std::uintptr_t Unavailable(std::uintptr_t a_src);
	};

	[[nodiscard]] Trampoline& GetTrampoline();
	void AllocTrampoline(std::size_t a_size, bool a_trySKSEReserve = true);
}
