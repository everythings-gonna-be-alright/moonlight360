/* xsdk has no ioctl(). ENet includes this header but, with fcntl() available (HAS_FCNTL), never calls ioctl(). */
#pragma once
int ioctl(int fd, unsigned long request, ...);
