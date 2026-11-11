#ifndef XV6_LOG_H
#define XV6_LOG_H

// Read-only inode operations do not use begin_op(). Admit them before taking
// inode/buffer locks and leave after the complete read/stat has been captured.
// Never hold admission across pipe/console input or nest begin_op() inside it.
void log_read_enter(void);
void log_read_leave(void);

#endif
