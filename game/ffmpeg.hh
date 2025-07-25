#pragma once

#include "chrono.hh"
#include "texture.hh"
#include "util.hh"
#include "libda/sample.hpp"

#include "aubio/aubio.h"

#include <fmt/format.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// ffmpeg forward declarations
extern "C" {
  #include <libavutil/avutil.h> // For AVMediaType

  struct AVChannelLayout;
  struct AVCodecContext;
  struct AVFormatContext;
  struct AVFrame;
  struct AVStream;
  void av_frame_free(AVFrame **);
  struct SwrContext;
  void swr_free(struct SwrContext **);
  void swr_close(struct SwrContext *);
  struct SwsContext;
  void sws_freeContext(struct SwsContext *);
}

/// ffmpeg class
class FFmpeg {
  public:
	// Exceptions thrown by class
	class Eof: public std::exception {};
	class Error : public std::runtime_error {
	  public:
		Error(const FFmpeg &self, int errorValue, const char *func): std::runtime_error(msgFmt(self, errorValue, func)) {}
		friend struct fmt::formatter<FFmpeg::Error>;
	  private:
		static std::string msgFmt(const FFmpeg &self, int errorValue, const char *func);
	};
	friend Error;

	void inline check(int errorCode, const char* func = "") {
		if (errorCode < 0) throw Error(*this, errorCode, func);
	};
	/// Decode file, depending on media type audio.
	FFmpeg(fs::path const& filename, int mediaType);

	void handleOneFrame();

	/** Seek to the chosen time. **/
	virtual void seek(double time);

	/// duration
	double duration() const;

	/// replay gain, in +/- decibels.  Can be zero, and is zero if not defined for the track
	double getReplayGainInDecibels() const;
	double getReplayGainVolumeFactor() const;
	double calculateLinearGain(double gainInDB) const;

	virtual ~FFmpeg() = default;

  protected:
	static void frameDeleter(AVFrame *f) { if (f) av_frame_free(&f); }
	void readReplayGain(const AVStream *stream);
	using uFrame = std::unique_ptr<AVFrame, std::integral_constant<decltype(&frameDeleter), &frameDeleter>>;

	virtual void processFrame(uFrame frame) = 0;

	void handleSomeFrames();

	static void avformat_close_input(AVFormatContext *fctx);
	static void avcodec_free_context(AVCodecContext *avctx);

	fs::path m_filename;
	double m_position = 0.0;
	double m_duration = 0.0;
	double m_replayGainDecibels = 0.0; ///< dB gain factor to normalise perceived loudness
	double m_replayGainFactor = 0.0;   ///< Replay Gain converted into a volume correction
	// libav-specific variables
	int m_streamId = -1;
	std::unique_ptr<AVFormatContext, decltype(&avformat_close_input)> m_formatContext{nullptr, avformat_close_input};
	std::unique_ptr<AVCodecContext, decltype(&avcodec_free_context)> m_codecContext{nullptr, avcodec_free_context};
};

#if !defined(__PRETTY_FUNCTION__) && defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#define FFMPEG_CHECKED(func, args, caller) FFmpeg::check(func args, caller)

class DurationFFmpeg : public FFmpeg {
  public:public:
	DurationFFmpeg(fs::path const& file) : FFmpeg(file, AVMEDIA_TYPE_AUDIO) {};
	void processFrame(uFrame) override { return; };
};

class AudioFFmpeg : public FFmpeg {
  public:
	using AudioCb = std::function<void(const std::int16_t *data, std::int64_t count, std::int64_t sample_position)>;
	AudioFFmpeg(fs::path const& file, int rate, AudioCb audioCb);

	void seek(double time) override;
  protected:
	void processFrame(uFrame frame) override;
  private:
	std::int64_t m_position_in_48k_frames = -1;
	int m_rate = 0;
	AudioCb handleAudioData;
	std::unique_ptr<SwrContext, void(*)(SwrContext*)> m_resampleContext{nullptr, [] (auto p) { swr_close(p); swr_free(&p); }};
};

class VideoFFmpeg : public FFmpeg {
  public:
	using VideoCb = std::function<void(Bitmap)>;
	VideoFFmpeg(fs::path const& file, VideoCb videoCb);

  protected:
	void processFrame(uFrame frame) override;
  private:
	std::unique_ptr<SwsContext, void(*)(SwsContext*)> m_swsContext{nullptr, sws_freeContext};
        VideoCb handleVideoData;

};

class AudioBuffer {
  public:
	using uFvec = std::unique_ptr<fvec_t, std::integral_constant<decltype(&del_fvec), &del_fvec>>;

	AudioBuffer(fs::path const& file, unsigned rate, size_t size = 4320256);
	~AudioBuffer();

	uFvec makePreviewBuffer();
	void operator()(const std::int16_t *data, std::int64_t count, std::int64_t sample_position);
	bool prepare(std::int64_t pos);
	bool read(float* begin, std::int64_t samples, std::int64_t pos, float volume = 1.0f);
	bool terminating();
	double duration();

  private:
	// must be called holding the mutex
	bool eof(std::int64_t pos) const {
		return (m_eof_pos != -1 && pos >= m_eof_pos) || (double(pos) / m_sps >= m_duration);
	}

	bool wantSeek();
	bool wantMore();
	/// Should the input stop waiting?
	bool condition();

	mutable std::mutex m_mutex;
	std::condition_variable m_cond;

	std::vector<std::int16_t> m_data;
	std::int64_t m_write_pos = 0;
	std::int64_t m_read_pos = 0;
	std::int64_t m_eof_pos = -1; // -1 until we get the read end from ffmpeg

	const unsigned m_sps;
	double m_duration{ 0 };
	double m_replayGainDecibels{ 0.0 };
	double m_replayGainFactor{ 0.0 };
	bool m_seek_asked { false };
	bool m_quit{ false };
	std::future<void> reader_thread;
};
