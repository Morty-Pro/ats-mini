1. you can run the BuildFirmware from Action Workflow of GitHub repo, then flash with esp-flash-tool

2. you can run the Arduino-CLI to compile and Upload to board, open the repo file and run command (change the COM_PORT to yours, like COM9)
arduino-cli compile --clean -e -p COM_PORT -u ats-mini

3. or build it using any CMake tools and upload, run commands in order(also change the PORT to yours) 
(optioal) set HALF_STEP=1 
(important) set PORT=COM5
(important) make upload