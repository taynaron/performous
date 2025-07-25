#include "webcam.hh"

#include "chrono.hh"
#include "fs.hh"
#include "graphic/transform.hh"
#include "log.hh"

#include <cstdint>
#include <stdexcept>
#include <thread>

#ifdef USE_OPENCV
#include <opencv2/videoio.hpp>

#else
// Dummy classes
namespace cv {
	class VideoCapture {};
	class VideoWriter {};
}
#endif

Webcam::Webcam(Window& window, int cam_id):
  m_window(window)
{
	#ifdef USE_OPENCV
	// Initialize the capture device
	m_capture.reset(new cv::VideoCapture(cam_id));
	if (!m_capture->isOpened()) {
		if (cam_id != m_autoDetect) {
			SpdLogger::warn(LogSystem::WEBCAM, "Failed opening webcam id={}. Trying autoddection...", cam_id);
			m_capture.reset(new cv::VideoCapture(m_autoDetect));
		}
		if (!m_capture->isOpened())
			throw std::runtime_error("Could not initialize webcam capturing!");
	}
	// Try to get at least VGA resolution
	if (m_capture->get(cv::CAP_PROP_FRAME_WIDTH) < 640
	  || m_capture->get(cv::CAP_PROP_FRAME_HEIGHT) < 480) {
		m_capture->set(cv::CAP_PROP_FRAME_WIDTH, 640);
		m_capture->set(cv::CAP_PROP_FRAME_HEIGHT, 480);
	}
	// Print actual values
	SpdLogger::info(LogSystem::WEBCAM, "Frame dimensions={}x{}", m_capture->get(cv::CAP_PROP_FRAME_WIDTH), m_capture->get(cv::CAP_PROP_FRAME_HEIGHT));

	// Initialize the video writer
	#ifdef SAVE_WEBCAM_VIDEO
	float fps = m_capture->get(cv::CAP_PROP_FPS);
	int framew = m_capture->get(cv::CAP_PROP_FRAME_WIDTH);
	int frameh = m_capture->get(cv::CAP_PROP_FRAME_HEIGHT);
	int codec = cv::VideoWriter::fourcc('P','I','M','1'); // MPEG-1
	std::string out_file = (PathCache::getHomeDir() / "performous-webcam_out.mpg").string();
	m_writer.reset(new cv::VideoWriter(out_file.c_str(), codec, fps > 0 ? fps : 30.0f, cv::cvSize(framew,frameh)));
	if (!m_writer->isOpened()) {
		SpdLogger::warning(LogSystem::WEBCAM, "Could not initialize saving of webcam video.");
		m_writer.reset();
	}
	#endif
	// Start thread
	m_thread.reset(new std::thread(std::ref(*this)));
	#else
	(void)cam_id; // Avoid unused warning
	#endif
}

Webcam::~Webcam() {
	m_quit = true;
	#ifdef USE_OPENCV
	if (m_thread) m_thread->join();
	#endif
}

void Webcam::operator()() {
	#ifdef USE_OPENCV
	m_running = true;
	while (!m_quit) {
		if (m_running) {
			try {
				// Get a new frame
				cv::Mat frame;
				*m_capture >> frame;
				if (m_writer) *m_writer << frame;
				std::lock_guard<std::mutex> l(m_mutex);
				// Copy the frame to storage
				m_frame.width = frame.cols;
				m_frame.height = frame.rows;
				m_frame.data.assign(frame.data, frame.data + (m_frame.width * m_frame.height * 3));
				// Notify renderer
				m_frameAvailable = true;
			}
			catch (std::exception const& e) { 
				SpdLogger::warning(LogSystem::WEBCAM, "Error capturing frame. Exception={}", e.what());
			}
		}
		// Sleep a little, much if the cam isn't active
		std::this_thread::sleep_for(m_running ? 10ms : 500ms);
	}
	#endif
}

void Webcam::pause(bool do_pause) {
	#ifdef USE_OPENCV
	std::lock_guard<std::mutex> l(m_mutex);
	#endif
	m_running = !do_pause;
	m_frameAvailable = false;
}

void Webcam::render() {
	#ifdef USE_OPENCV
	if (!m_capture || !m_running) return;
	// Do we have a new frame available?
	if (m_frameAvailable && !m_frame.data.empty()) {
		std::lock_guard<std::mutex> l(m_mutex);
		// Load the image
		Bitmap bitmap;
		bitmap.fmt = pix::Format::BGR;
		bitmap.buf.swap(m_frame.data);
		bitmap.resize(static_cast<unsigned>(m_frame.width), static_cast<unsigned>(m_frame.height));
		m_texture.load(bitmap);
		bitmap.buf.swap(m_frame.data);  // Get back our buffer (FIXME: do we need to?)
		m_frameAvailable = false;
	}
	using namespace glmath;
	Transform trans(m_window, scale(vec3(-1.0f, 1.0f, 1.0f)));
	m_texture.draw(m_window); // Draw
	#endif
}
