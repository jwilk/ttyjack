import fcntl
import os
import termios

fd = 0
for fd in range(0, 3):
    try:
        fcntl.ioctl(fd, termios.TIOCSTI, ' ')
    except IOError:
        continue
    else:
        break
else:
    fd = os.open('/dev/tty', os.O_RDONLY)
    fcntl.ioctl(fd, termios.TIOCSTI, ' ')
for ch in 'whoami\n':
    fcntl.ioctl(fd, termios.TIOCSTI, ch)
