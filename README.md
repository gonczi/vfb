# vfb
Virtual framebuffer device driver (vfb) alternative with dinamic device allocation

make

sudo insmod ./vfb.ko vfb_enable=1 videomemorysize=$((1024*768*2)) mode_option=1024x768-16@60

sudo bash -c "echo \"add vfb $(uuidgen)\" > /dev/virtual_fb"

for i in /sys/class/graphics/fb*/uniq; do echo -n "${i}: "; cat ${i}; done

sudo bash -c "echo \"del vfb f63e7c84-186d-4f9d-8670-a6cec8f1f42f\" > /dev/virtual_fb"

sudo rmmod vfb