#include "wingman_uploader.h"
#include "addon.h"
#include "settings.h"

IMPLEMENT_MODULE(WingmanUploader, wingman_uploader)

void WingmanUploader::add_log(std::shared_ptr<Log> log)
{
	if (!initialized.load())
		return;

	std::unique_lock lock(log->mutex);

	if (log->parser_data.status != ParseStatus::PARSED || (log->wingman_upload.status != UploadStatus::AVAILABLE && log->wingman_upload.status != UploadStatus::FAILED))
	{
		addon::log("Log unavailable for wingman upload: " + log->id, LOGLEVEL_WARNING);
		return;
	}

	log->wingman_upload.status = UploadStatus::QUEUED;

	log->update_view();

	{
		std::unique_lock upload_queue_lock(upload_queue_mutex);
		upload_queue.push(log);
		upload_cv.notify_one();
	}
}

void WingmanUploader::process_auto_upload(std::shared_ptr<Log> log)
{
	auto settings = addon::settings->get().wingman;

	if (!settings.auto_upload)
		return;

	std::shared_lock lock(log->mutex);

	auto log_trigger_id = log->trigger_id;

	if (log->parser_data.status != ParseStatus::PARSED || (settings.auto_upload_filter == AutoUploadFilter::SUCCESSFUL_ONLY && !log->parser_data.encounter.success))
		return;

	lock.unlock();

	bool is_in_filter = std::find(settings.auto_upload_encounters.begin(), settings.auto_upload_encounters.end(), log_trigger_id) != settings.auto_upload_encounters.end();

	if (is_in_filter)
		add_log(log);
}

void WingmanUploader::run()
{
	addon::log("Starting Wingman uploader", LOGLEVEL_DEBUG);

	while (true)
	{
		std::unique_lock upload_queue_lock(upload_queue_mutex);

		upload_cv.wait(upload_queue_lock, [&]() { return !upload_queue.empty() || !initialized.load(); });

		if (!initialized.load())
			break;

		auto log = upload_queue.front();
		upload_queue.pop();

		upload_queue_lock.unlock();

		std::unique_lock lock(log->mutex);

		if (log->parser_data.status != ParseStatus::PARSED || log->wingman_upload.status != UploadStatus::QUEUED)
		{
			addon::log("Log unavailable for wingman upload: " + log->id, LOGLEVEL_WARNING);
			log->wingman_upload.status = UploadStatus::FAILED;
			continue;
		}

		log->wingman_upload.status = UploadStatus::UPLOADING;

		log->update_view();

		auto id = log->id;
		auto log_data = log->get_data();

		lock.unlock();

		auto upload = this->upload(log_data);
		addon::log("Wingman upload successful: " + id, LOGLEVEL_INFO);

		lock.lock();

		log->wingman_upload = upload;
		log->update_view();
	}

	addon::log("Wingman uploader stopped", LOGLEVEL_DEBUG);
}

WingmanUpload WingmanUploader::upload(LogData&)
{
	WingmanUpload upload;
	upload.status = UploadStatus::UPLOADED;
	return upload;
}

bool WingmanUploader::get_server_availability()
{
	servers_available.store(true);
	return true;
}
