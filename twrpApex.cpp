#include "twrpApex.hpp"
#include "twrp-functions.hpp"

namespace fs = std::filesystem;

bool twrpApex::loadApexImages() {
	std::vector<std::string> apexFiles;
	if (access(APEX_DIR, F_OK) != 0) {
		LOGERR("Unable to open %s\n", APEX_DIR);
		return false;
	}
	for (const auto& entry : fs::directory_iterator(APEX_DIR)) {
	   if (entry.is_regular_file()) {
		   apexFiles.push_back(entry.path().string());
	   }
	}

	if (apexFiles.size() == 0) {
		// flattened apex directory
		LOGINFO("Bind mounting flattened apex directory\n");
		if (mount(APEX_DIR, APEX_BASE, "", MS_BIND, NULL) < 0) {
			LOGERR("Unable to bind mount flattened apex directory\n");
			return false;
		}
		return true;
	}
	if (!mountApexOnLoopbackDevices(apexFiles)) {
		LOGERR("Unable to create loop devices to mount apex files\n");
		return false;
	}

	return true;
}

std::string twrpApex::unzipImage(std::string file) {
	ZipArchiveHandle handle;
	int32_t ret = OpenArchive(file.c_str(), &handle);
	if (ret != 0) {
		LOGERR("unable to open zip archive %s\n", file.c_str());
		CloseArchive(handle);
		return nullptr;
	}

	ZipEntry entry;
	std::string zip_string(APEX_PAYLOAD);
	ret = FindEntry(handle, zip_string, &entry);
	if (ret != 0) {
		LOGERR("unable to find %s in zip\n", APEX_PAYLOAD);
		CloseArchive(handle);
		return nullptr;
	}

	std::string baseFile = basename(file.c_str());
	std::string path("/tmp/");
	path = path + baseFile;
	int fd = open(path.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
	ret = ExtractEntryToFile(handle, &entry, fd);
	if (ret != 0) {
		LOGERR("unable to extract %s\n", path.c_str());
		close(fd);
		CloseArchive(handle);
		return nullptr;
	}

	CloseArchive(handle);
	close(fd);
	return path;
}

bool twrpApex::mountApexOnLoopbackDevices(std::vector<std::string> apexFiles) {
	int fd = open(LOOP_CONTROL, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		LOGERR("Unable to open %s device. Reason: %s\n", LOOP_CONTROL, strerror(errno));
		return false;
	}

	size_t device_no = 0;
	for (auto&& apexFile:apexFiles) {
		int num = ioctl(fd, LOOP_CTL_GET_FREE);
		std::string loop_device = LOOP_BLOCK_DEVICE_DIR;
		loop_device = loop_device + "loop" + std::to_string(num);
		if (!TWFunc::Path_Exists(loop_device)) {
			int ret = mknod(loop_device.c_str(), S_IFBLK | S_IRUSR | S_IWUSR , makedev(7, device_no));
			if (ret != 0) {
				LOGERR("Unable to create loop device: %s\n", loop_device.c_str());
				return false;
			}
		}
		std::string fileToMount = unzipImage(apexFile);
		bool load_result = loadApexImage(fileToMount, device_no);
		if (!load_result) {
			return false;
		}
		device_no++;
	}
	return true;
}

bool twrpApex::loadApexImage(std::string fileToMount, size_t loop_device_number) {
	struct loop_info64 info;

	int fd = open(fileToMount.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		LOGERR("unable to open apex file: %s. Reason: %s\n", fileToMount.c_str(), strerror(errno));
		return false;
	}

	std::string loop_device = "/dev/block/loop" + std::to_string(loop_device_number);
	int loop_fd = open(loop_device.c_str(), O_RDONLY);
	if (loop_fd < 0) {
		LOGERR("unable to open loop device: %s\n", loop_device.c_str());
		close(fd);
		return false;
	}

	if (ioctl(loop_fd, LOOP_SET_FD, fd) < 0) {
		LOGERR("failed to mount %s to loop device %s\n", fileToMount.c_str(), loop_device.c_str());
		close(fd);
		close(loop_fd);
		return false;
	}

	close(fd);

	memset(&info, 0, sizeof(struct loop_info64));
	strlcpy((char*)info.lo_crypt_name, "twrpApex", LO_NAME_SIZE);
	off_t apex_size = lseek(fd, 0, SEEK_END);
	info.lo_sizelimit = apex_size;
	if (ioctl(loop_fd, LOOP_SET_STATUS64, &info)) {
		LOGERR("failed to mount loop: %s: %s\n", fileToMount.c_str(), strerror(errno));
		close(loop_fd);
		return false;
	}
	if (ioctl(loop_fd, BLKFLSBUF, 0) == -1) {
		LOGERR("Unable to flush loop device buffers\n");
		return false;
	}
	if (ioctl(loop_fd, LOOP_SET_BLOCK_SIZE, 4096) == -1) {
		LOGINFO("Failed to set DIRECT_IO buffer size\n");
	}
	close(loop_fd);

	std::string bind_mount(APEX_BASE);
	bind_mount = bind_mount + basename(fileToMount.c_str());

	int ret = mkdir(bind_mount.c_str(), 0666);
	if (ret != 0) {
		LOGERR("Unable to create bind mount directory: %s\n", bind_mount.c_str());
		return false;
	}

	ret = mount(loop_device.c_str(), bind_mount.c_str(), "ext4", MS_RDONLY, nullptr);
	if (ret != 0) {
		LOGERR("unable to mount loop device %s to %s. Reason: %s\n", loop_device.c_str(), bind_mount.c_str(), strerror(errno));
		return false;
	}

	return true;
}
