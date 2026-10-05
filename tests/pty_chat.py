#!/usr/bin/env python3
"""Optional development check: real ncurses PTY, no live model or daemon needed."""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time

binary = sys.argv[1] if len(sys.argv) > 1 else './build/saga_chat_ui_tests'
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(TERM='xterm-256color', LANG='C.UTF-8', LINES='10', COLUMNS='30')
    os.execv(binary, [binary, '--demo'])

output = bytearray()
def collect(seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        ready, _, _ = select.select([fd], [], [], min(.05, max(0, until-time.monotonic())))
        if ready:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            output.extend(chunk)

def resize(rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    os.kill(pid, signal.SIGWINCH)

try:
    resize(28, 90)
    collect(.4)
    assert b'Bold' in output and b'**Bold**' not in output
    assert b'\x1b[3m' in output, 'italic attribute not emitted'
    assert b'48;5;236m' in output, 'inline code background not emitted'
    os.write(fd, 'hola 東京\r'.encode())
    collect(.6)
    assert b'Awaiting approval' in output
    assert b'38;5;220m' in output, 'approval border is not yellow'
    assert b'38;5;67m' in output, 'shell prompt is not muted blue'
    assert b'38;5;73m"command"' in output, 'approval JSON key is not cyan'
    assert b'38;5;180m"' in output, 'approval JSON string quotes are not amber'
    assert b'38;5;114mtmux' in output, 'approval command does not use shell command highlighting'
    os.write(fd, b'/help permissions\r')
    collect(.2)
    assert b'Examples:' in output
    assert b'**/permissions' not in output and b'```bash' not in output
    assert b'38;5;73m' in output, 'help command accent is missing'
    assert b'38;5;250m' in output, 'help description gray is missing'
    os.write(fd, b'/status\r')
    collect(.2)
    assert b'not available' in output, 'local status did not render while approval was pending'
    os.write(fd, b'/permissions\r')
    collect(.2)
    assert b'Choose 1, 2, 3 or 4' in output, 'permission menu is blocked while busy'
    os.write(fd, b'3\r')
    collect(.2)
    assert b'LIVE_PERMISSION_UPDATED' in output, 'numeric permission selection is blocked while busy'
    os.write(fd, b'/tasks\r')
    collect(.2)
    assert b'LIVE_TASKS_INSPECTED' in output, 'inspection command is blocked while busy'
    os.write(fd, b'/compact\r')
    collect(.2)
    assert b'LIVE_COMMAND_REJECTED' in output, 'unsafe live command did not report a local error'
    before_resize=len(output)
    resize(7, 12)
    collect(.2)
    resize(34, 110)
    collect(.2)
    assert b'\x1b[2J' in output[before_resize:], 'resize did not force a full screen repaint'
    os.write(fd, b'y\r')
    collect(.8)
    assert b'Approved.' in output
    assert b'21.9K/65.5K' in output
    assert b'All done.' in output and b'**All done.**' not in output
    os.write(fd, b'\x1b[5~\x1b[6~')
    collect(.2)
    os.write(fd, b'/exit\r')
    collect(.4)
    _, status = os.waitpid(pid, 0)
    pid = 0
    assert os.waitstatus_to_exitcode(status) == 0
    assert b'NCURSES_EXIT_OK' in output
    print('PASS real PTY: Markdown styles, colors, Unicode, approval Enter, help/status, resize and scrolling')
finally:
    if pid:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    os.close(fd)

# Check SGR wheel events, matching the protocol enabled for xterm-256color.
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(TERM='xterm-256color', LANG='C.UTF-8')
    os.execv(binary, [binary, '--mouse-demo'])
output.clear()
try:
    resize(28, 90)
    collect(.4)
    assert b'HISTORY_LATEST' in output and b'HISTORY_OLDEST' not in output
    assert b'1000h' in output, 'terminal mouse reporting was not enabled'
    os.write(fd, b'pending draft')
    os.write(fd, b'\x1b[<64;5;3M' * 30)
    collect(.4)
    before_resize = len(output)
    resize(29, 90)
    collect(.2)
    top = output[before_resize:]
    assert b'HISTORY_OLDEST' in top and b'HISTORY_LATEST' not in top
    assert b'pending draft' in top and b'0.0K/65.5K' in top, 'wheel moved or cleared the footer/input'
    os.write(fd, b'\x1b[<65;5;3M' * 30)
    collect(.4)
    before_resize = len(output)
    resize(28, 90)
    collect(.2)
    bottom = output[before_resize:]
    assert b'HISTORY_LATEST' in bottom and b'HISTORY_OLDEST' not in bottom
    assert b'pending draft' in bottom and b'0.0K/65.5K' in bottom
    os.write(fd, b'\x15background\r')
    collect(.1)
    os.write(fd, b'copy draft\x1bOQ')
    before_selection=len(output)
    collect(.15)
    assert b'1000l' in output[before_selection:] and b'Select text' in output[before_selection:]
    paused=len(output)
    collect(.6)
    assert len(output)==paused, 'selection mode repainted while an update arrived'
    resize(29, 90)
    collect(.2)
    assert b'copy draft' in output[paused:] and b'COPY_QUEUED_UPDATE' not in output[paused:]
    before_resume=len(output)
    os.write(fd,b'\x1bOQ')
    collect(.3)
    assert b'1000h' in output[before_resume:] and b'COPY_QUEUED_UPDATE' in output[before_resume:]
    resize(28,90)
    collect(.2)
    assert b'copy draft' in output[before_resume:]
    os.write(fd, b'\x15/exit\r')
    collect(.4)
    _, status = os.waitpid(pid, 0)
    pid = 0
    assert os.waitstatus_to_exitcode(status) == 0 and b'MOUSE_EXIT_OK' in output
    assert b'1000l' in output, 'terminal mouse reporting was not disabled on exit'
    print('PASS real PTY: mouse wheel, selection mode, paused repaint, queued updates and preserved footer/input')
finally:
    if pid:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    os.close(fd)

# Complete diff proposals: mouse expansion, resize, keyboard approval/rejection,
# automatic transcript expansion, and cancellation while F2 freezes rendering.
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(TERM='xterm-256color', LANG='C.UTF-8')
    os.execv(binary, [binary, '--diff-demo'])
output.clear()
def click(col, row):
    os.write(fd, f'\x1b[<0;{col};{row}M\x1b[<0;{col};{row}m'.encode())
try:
    resize(28, 90)
    collect(.3)
    os.write(fd, b'review\r')
    collect(.4)
    assert b'Approve file edit' in output and b'See more' in output
    assert b'38;5;114m+new' in output and b'38;5;174m-old' in output
    assert b'new 15' not in output, 'proposal did not start collapsed'
    # 17-row popup centered in the 23-row chat area; third button is row 19.
    before_click=len(output)
    click(6, 19)
    collect(.3)
    resize(29, 90)
    collect(.2)
    assert b'See less' in output[before_click:], 'See more mouse target did not expand'
    os.write(fd, b'\x1b[<65;5;3M' * 20)
    collect(.3)
    assert b'new 15' in output, 'expanded diff did not scroll to its end'
    resize(34, 100)
    collect(.3)
    assert b'chat with' in output and b'0.0K/65.5K' in output
    os.write(fd, b'\r')  # Focus starts on Approve; Tab/Enter also reaches other buttons.
    collect(.4)
    assert b'DIFF_ACCEPTED' in output
    # The applied diff follows the user bubble and approval marker.
    before_click=len(output)
    click(6, 18)
    collect(.3)
    resize(35,100)
    collect(.2)
    assert b'See less' in output[before_click:], 'applied diff mouse target did not expand'
    os.write(fd, b'reject\r')
    collect(.3)
    os.write(fd, b'\t\r')
    collect(.4)
    assert b'DIFF_REJECTED' in output, 'Tab/Enter did not activate Reject'
    os.write(fd, b'cancel\r')
    collect(.2)
    assert b'LIVE_OUTPUT_STARTED' in output
    os.write(fd, b'\x1bOQ')
    collect(.2)
    assert b'Select text' in output
    os.write(fd, b'\x03')
    collect(.4)
    assert b'STOPPED_COPY_WORK' in output, 'Ctrl+C in copy mode did not stop active work'
    os.write(fd, b'/exit\r')
    collect(.3)
    _, status=os.waitpid(pid,0)
    pid=0
    assert os.waitstatus_to_exitcode(status)==0 and b'DIFF_EXIT_OK' in output
    print('PASS real PTY: complete diff popup, mouse/keyboard actions, resize, transcript expansion and stop in F2')
finally:
    if pid:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid,0)
    os.close(fd)
