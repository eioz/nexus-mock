#include "dps_report_uploader.h"
#include "addon.h"
#include "platform/platform.h"
#include "settings.h"

IMPLEMENT_MODULE(DPSReportUploader, dps_report_uploader)

void DPSReportUploader::add_log(std::shared_ptr<Log> log)
{
	if (!initialized.load())
		return;

	std::unique_lock lock(log->mutex);

	if (log->dps_report_upload.status != UploadStatus::AVAILABLE && log->dps_report_upload.status != UploadStatus::FAILED)
	{
		addon::log("Log unavailable for dps.report upload: " + log->id, LOGLEVEL_WARNING);
		return;
	}

	log->dps_report_upload.status = UploadStatus::QUEUED;
	log->update_view();

	{
		std::unique_lock upload_queue_lock(upload_queue_mutex);
		upload_queue.push(log);
		upload_cv.notify_one();
	}
}

void DPSReportUploader::process_auto_upload(std::shared_ptr<Log> log)
{
	auto settings = addon::settings->get().dps_report;

	if (!settings.auto_upload)
		return;

	std::shared_lock lock(log->mutex);

	if (log->dps_report_upload.status != UploadStatus::AVAILABLE)
		return;

	if (settings.auto_upload_filter == AutoUploadFilter::SUCCESSFUL_ONLY && !log->parser_data.is_success())
		return;

	auto log_trigger_id = log->trigger_id;
	lock.unlock();

	bool is_in_filter = std::find(settings.auto_upload_encounters.begin(), settings.auto_upload_encounters.end(), log_trigger_id) != settings.auto_upload_encounters.end();

	if (is_in_filter)
		add_log(log);
}

void DPSReportUploader::run()
{
	addon::log("Starting dps.report uploader", LOGLEVEL_DEBUG);

	while (initialized.load())
	{
		std::unique_lock upload_queue_lock(upload_queue_mutex);
		upload_cv.wait(upload_queue_lock, [&]() { return !upload_queue.empty() || !initialized.load(); });

		if (!initialized.load())
			break;

		auto log = upload_queue.front();
		upload_queue.pop();
		upload_queue_lock.unlock();

		std::unique_lock lock(log->mutex);
		if (log->dps_report_upload.status != UploadStatus::QUEUED)
		{
			addon::log("Log unavailable for dps.report upload: " + log->id, LOGLEVEL_WARNING);
			continue;
		}

		log->dps_report_upload.status = UploadStatus::UPLOADING;
		log->update_view();

		auto id = log->id;
		auto evtc_file_path = log->evtc_file_path;
		lock.unlock();

		auto upload = this->upload(evtc_file_path);
		addon::log("Uploaded " + id + " to dps.report: " + upload.url, LOGLEVEL_INFO);

		if (addon::settings->get().dps_report.auto_upload_copy_url_to_clipboard)
			addon::platform::copy_text_to_clipboard(upload.url);

		lock.lock();
		log->dps_report_upload = upload;
		log->update_view();
	}

	addon::log("Stopping dps.report uploader", LOGLEVEL_DEBUG);
}

DpsReportUpload DPSReportUploader::upload(std::filesystem::path evtc_file_path)
{
	DpsReportUpload upload;
	auto id = evtc_file_path.stem().string();
	if (id.empty())
		id = "mock-log";
	upload.id = "mock-" + id;
	upload.url = "mock://dps-report/" + upload.id;
	upload.user_token = "mock-user-token";
	upload.status = UploadStatus::UPLOADED;
	return upload;
}
