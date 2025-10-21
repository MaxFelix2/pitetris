#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <linux/input.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
//to test different things
//
 #define TARGET_FB_NAME "RPi-Sense FB" 

int open_framebuffer_by_name(const char *target_name) {
    struct dirent *entry;
    DIR *dp = opendir("/sys/class/graphics");
    if (!dp) {
        perror("opendir");
        return -1;
    }

    while ((entry = readdir(dp)) != NULL) {
        if (strncmp(entry->d_name, "fb", 2) != 0)
            continue; // Skip non-fb entries

        // Build path to name file
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/graphics/%s/name", entry->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char fb_name[128];
        if (fgets(fb_name, sizeof(fb_name), f)) {
            // Remove trailing newline
            fb_name[strcspn(fb_name, "\n")] = 0;

            if (strcmp(fb_name, target_name) == 0) {
                // Found matching framebuffer
                fclose(f);
                closedir(dp);

                // Build /dev/fbN path
                char devpath[128];
                snprintf(devpath, sizeof(devpath), "/dev/%s", entry->d_name);
                int fd = open(devpath, O_RDWR);
                if (fd == -1) {
                    perror("open framebuffer");
                    return -1;
                }
                return fd;
            }
        }
        fclose(f);
    }

    closedir(dp);
    fprintf(stderr, "Framebuffer \"%s\" not found\n", target_name);
    return -1;
}

int main(int argc, char *argv[])
{ 
  int fd = open_framebuffer_by_name(TARGET_FB_NAME);
  printf("Found fb: %d", fd);
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  ioctl(fd, FBIOGET_FSCREENINFO, &fix);
  ioctl(fd, FBIOGET_VSCREENINFO, &var);
  printf("Resolution: %ux%u\n", var.xres, var.yres);
  printf("Virtual resolution: %ux%u\n", var.xres_virtual, var.yres_virtual);
  printf("Bits per pixel: %u\n", var.bits_per_pixel);
  printf("Line length (bytes): %u\n", fix.line_length);
  printf("Framebuffer mem size (smem_len): %u\n", fix.smem_len);
  printf("Type: id=%d, type=%d, visual=%d\n", fix.id[0], fix.type, fix.visual);
  //Light up screen

  close(fd);
  return EXIT_SUCCESS;
}
