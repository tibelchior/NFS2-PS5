FROM --platform=linux/amd64 fedora:43
RUN dnf -y install cmake ninja-build clang lld gcc gcc-c++ make SDL2-devel mesa-libGL-devel mesa-libEGL-devel mesa-dri-drivers gdb p7zip-plugins bsdtar python3 xorg-x11-server-Xvfb && dnf clean all
RUN dnf -y install SDL3 SDL3-devel && dnf clean all
RUN dnf -y install mesa-vulkan-drivers vulkan-loader vulkan-loader-devel vulkan-headers vulkan-tools xwd ImageMagick && dnf clean all
RUN dnf -y install xdotool && dnf clean all
RUN dnf -y install x11vnc novnc python3-websockify && dnf clean all
