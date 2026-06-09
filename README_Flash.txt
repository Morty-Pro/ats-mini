Project directory = \gitRepo\ats-mini
BUILD files directory = \ats-mini\build\esp32.esp32.esp32s3

1. you can run the BuildFirmware from Action Workflow of GitHub repo (runs every time you push it from VScode), then flash with esp-flash-tool

(BUILD directory, then in CMD run:) python -m esptool --chip esp32-s3 --port COM10 --baud 921600 write_flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 ats-mini.ino.bootloader.bin 0x8000 ats-mini.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 ats-mini.ino.bin

2. (Project directory, run this command in CMD) you can run the Arduino-CLI to compile and Upload to board, open the repo file and run command (change the COM_PORT to yours, like COM9)

dir: from anywhere you can use commands  (it runs automatically in gitRepo\ats-mini\ats-mini) (to see CMD profile (run): notepad $PROFILE)
{

	(minimal compile) 	  atsbuild
        (minimal compile-fast)    atsbuildfast

	(minimal upload) 	  atsupload COM10
        (minimal upload-fast)     atsuploadfast COM10

	(minimal copile & upload) atsflash COM10
        (minimal flash-fast)      atsflashfast COM10
}

dir: gitRepo\ats-mini\ats-mini

(Good for compile) arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none" --build-path build .
(Good for Upload) arduino-cli upload -p COM7 --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,CPUFreq=80,USBMode=hwcdc,FlashMode=qio,PartitionScheme=custom,DebugLevel=none"

(just compile)    arduino-cli compile -e --clean ats-mini
(build the files) arduino-cli compile --profile esp32s3-ospi --build-path build
(compile and upload) arduino-cli compile --clean -e -p COM10 -u ats-mini
(much faster build) arduino-cli compile --build-path build --fqbn esp32:esp32:esp32s3
(export binary) arduino-cli compile
(export binary for release) arduino-cli compile --export-binaries
(Upload to COMPort) arduino-cli upload -p COM7 --fqbn esp32:esp32:esp32s3
(COMPort available) arduino-cli board list
(compile from terminal) arduino-cli compile --fqbn esp32:esp32:esp32s3


3. (Directory = ats-mini\ats-mini)(this method is more usable in Github site not windows, this command use Arduino-CLI) build it using any CMake tools and upload, run commands in order(also change the PORT to yours), better to run it in powerShell

(optioal) set HALF_STEP=1 
(important) set PORT=COM5
(important) make upload