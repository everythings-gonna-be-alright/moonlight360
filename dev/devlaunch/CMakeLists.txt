# devlaunch: uploads logs to dev/devserver.py and downloads + launches the current build of a program.
cmake_minimum_required(VERSION 3.25)
project(devlaunch C)
add_executable(devlaunch main.c ../../app/src/posix_stubs.c)
set_target_properties(devlaunch PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(devlaunch PRIVATE -O2 -Wall -Wextra)
xsdk_add_xex(devlaunch NAME "Moonlight 360 dev launcher" VERSION 0.1.0.0)
