// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "uring.h"
#include "selector.h"

#include <liburing.h>
#include <poll.h>
#include <time.h>

#include "pidfd.c"

static const int DEBUG = 0;

static VALUE IO_Event_Selector_URing = Qnil;

enum {URING_ENTRIES = 64};

struct IO_Event_Selector_URing {
	struct IO_Event_Selector backend;
	struct io_uring ring;
	size_t pending;
	int blocked;
};

void IO_Event_Selector_URing_Type_mark(void *_data)
{
	struct IO_Event_Selector_URing *data = _data;
	IO_Event_Selector_mark(&data->backend);
}

static
void close_internal(struct IO_Event_Selector_URing *data) {
	if (data->ring.ring_fd >= 0) {
		io_uring_queue_exit(&data->ring);
		data->ring.ring_fd = -1;
	}
}

void IO_Event_Selector_URing_Type_free(void *_data)
{
	struct IO_Event_Selector_URing *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t IO_Event_Selector_URing_Type_size(const void *data)
{
	return sizeof(struct IO_Event_Selector_URing);
}

static const rb_data_type_t IO_Event_Selector_URing_Type = {
	.wrap_struct_name = "IO_Event::Backend::URing",
	.function = {
		.dmark = IO_Event_Selector_URing_Type_mark,
		.dfree = IO_Event_Selector_URing_Type_free,
		.dsize = IO_Event_Selector_URing_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE IO_Event_Selector_URing_allocate(VALUE self) {
	struct IO_Event_Selector_URing *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, Qnil);
	data->ring.ring_fd = -1;
	
	data->pending = 0;
	data->blocked = 0;
	
	return instance;
}

VALUE IO_Event_Selector_URing_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, loop);
	int result = io_uring_queue_init(URING_ENTRIES, &data->ring, 0);
	
	if (result < 0) {
		rb_syserr_fail(-result, "io_uring_queue_init");
	}
	
	rb_update_max_fd(data->ring.ring_fd);
	
	return self;
}

VALUE IO_Event_Selector_URing_loop(VALUE self) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return data->backend.loop;
}

VALUE IO_Event_Selector_URing_close(VALUE self) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE IO_Event_Selector_URing_transfer(VALUE self)
{
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_URing_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return IO_Event_Selector_resume(&data->backend, argc, argv);
}

VALUE IO_Event_Selector_URing_yield(VALUE self)
{
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return IO_Event_Selector_yield(&data->backend);
}

VALUE IO_Event_Selector_URing_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	IO_Event_Selector_queue_push(&data->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_URing_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return IO_Event_Selector_raise(&data->backend, argc, argv);
}
	
	int blocked;
VALUE IO_Event_Selector_URing_ready_p(VALUE self) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

static
int io_uring_submit_flush(struct IO_Event_Selector_URing *data) {
	if (data->pending) {
		if (DEBUG) fprintf(stderr, "io_uring_submit_flush(pending=%ld)\n", data->pending);
		
		// Try to submit:
		int result = io_uring_submit(&data->ring);
		
		if (result >= 0) {
			// If it was submitted, reset pending count:
			data->pending = 0;
		} else if (result != -EBUSY && result != -EAGAIN) {
			rb_syserr_fail(-result, "io_uring_submit_flush");
		}
		
		return result;
	}
	
	return 0;
}

static
int io_uring_submit_now(struct IO_Event_Selector_URing *data) {
	while (true) {
		int result = io_uring_submit(&data->ring);
		
		if (result >= 0) {
			data->pending = 0;
			return result;
		}
		
		if (result == -EBUSY || result == -EAGAIN) {
			IO_Event_Selector_yield(&data->backend);
		} else {
			rb_syserr_fail(-result, "io_uring_submit_now");
		}
	}
}

static
void io_uring_submit_pending(struct IO_Event_Selector_URing *data) {
	data->pending += 1;
}

struct io_uring_sqe * io_get_sqe(struct IO_Event_Selector_URing *data) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	while (sqe == NULL) {
		// The submit queue is full, we need to drain it:	
		io_uring_submit_now(data);
		
		sqe = io_uring_get_sqe(&data->ring);
	}
	
	return sqe;
}

struct process_wait_arguments {
	struct IO_Event_Selector_URing *data;
	pid_t pid;
	int flags;
	int descriptor;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return IO_Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	close(arguments->descriptor);
	
	return Qnil;
}

VALUE IO_Event_Selector_URing_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = NUM2INT(flags),
	};
	
	process_wait_arguments.descriptor = pidfd_open(process_wait_arguments.pid, 0);
	rb_update_max_fd(process_wait_arguments.descriptor);
	
	struct io_uring_sqe *sqe = io_get_sqe(data);

	if (DEBUG) fprintf(stderr, "IO_Event_Selector_URing_process_wait:io_uring_prep_poll_add(%p)\n", (void*)fiber);
	io_uring_prep_poll_add(sqe, process_wait_arguments.descriptor, POLLIN|POLLHUP|POLLERR);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_pending(data);

	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

static inline
short poll_flags_from_events(int events) {
	short flags = 0;
	
	if (events & IO_EVENT_READABLE) flags |= POLLIN;
	if (events & IO_EVENT_PRIORITY) flags |= POLLPRI;
	if (events & IO_EVENT_WRITABLE) flags |= POLLOUT;
	
	flags |= POLLERR;
	flags |= POLLHUP;
	
	return flags;
}

static inline
int events_from_poll_flags(short flags) {
	int events = 0;
	
	if (flags & POLLIN) events |= IO_EVENT_READABLE;
	if (flags & POLLPRI) events |= IO_EVENT_PRIORITY;
	if (flags & POLLOUT) events |= IO_EVENT_WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct IO_Event_Selector_URing *data;
	VALUE fiber;
	short flags;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct IO_Event_Selector_URing *data = arguments->data;
	
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	if (DEBUG) fprintf(stderr, "io_wait_rescue:io_uring_prep_poll_remove(%p)\n", (void*)arguments->fiber);
	
	io_uring_prep_poll_remove(sqe, (void*)arguments->fiber);
	io_uring_submit_now(data);

	rb_exc_raise(exception);
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct IO_Event_Selector_URing *data = arguments->data;

	VALUE result = IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
	if (DEBUG) fprintf(stderr, "io_wait:IO_Event_Selector_fiber_transfer -> %d\n", RB_NUM2INT(result));

	// We explicitly filter the resulting events based on the requested events.
	// In some cases, poll will report events we didn't ask for.
	short flags = arguments->flags & NUM2INT(result);
	
	return INT2NUM(events_from_poll_flags(flags));
};

VALUE IO_Event_Selector_URing_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	short flags = poll_flags_from_events(NUM2INT(events));
	
	if (DEBUG) fprintf(stderr, "IO_Event_Selector_URing_io_wait:io_uring_prep_poll_add(descriptor=%d, flags=%d, fiber=%p)\n", descriptor, flags, (void*)fiber);
	
	io_uring_prep_poll_add(sqe, descriptor, flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	
	// If we are going to wait, we assume that we are waiting for a while:
	io_uring_submit_pending(data);
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.fiber = fiber,
		.flags = flags
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

static int io_read(struct IO_Event_Selector_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	struct io_uring_sqe *sqe = io_get_sqe(data);

	if (DEBUG) fprintf(stderr, "io_read:io_uring_prep_read(fiber=%p)\n", (void*)fiber);

	io_uring_prep_read(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_now(data);
	
	VALUE result = IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
	if (DEBUG) fprintf(stderr, "io_read:IO_Event_Selector_fiber_transfer -> %d\n", RB_NUM2INT(result));

	return RB_NUM2INT(result);
}

VALUE IO_Event_Selector_URing_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	void *base;
	size_t size;
	rb_io_buffer_get_mutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	while (length > 0) {
		size_t maximum_size = size - offset;
		int result = io_read(data, fiber, descriptor, (char*)base+offset, maximum_size);
		
		if (result == 0) {
			break;
		} else if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			IO_Event_Selector_URing_io_wait(self, fiber, io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

static
int io_write(struct IO_Event_Selector_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	if (DEBUG) fprintf(stderr, "io_write:io_uring_prep_write(fiber=%p)\n", (void*)fiber);

	io_uring_prep_write(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_pending(data);
	
	int result = RB_NUM2INT(IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL));
	if (DEBUG) fprintf(stderr, "io_write:IO_Event_Selector_fiber_transfer -> %d\n", result);

	return result;
}

VALUE IO_Event_Selector_URing_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	const void *base;
	size_t size;
	rb_io_buffer_get_immutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (length > 0) {
		int result = io_write(data, fiber, descriptor, (char*)base+offset, length);
		
		if (result >= 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			IO_Event_Selector_URing_io_wait(self, fiber, io, RB_INT2NUM(IO_EVENT_WRITABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

#endif

static const int ASYNC_CLOSE = 1;

VALUE IO_Event_Selector_URing_io_close(VALUE self, VALUE io) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);

	if (ASYNC_CLOSE) {
		struct io_uring_sqe *sqe = io_get_sqe(data);
		
		io_uring_prep_close(sqe, descriptor);
		io_uring_sqe_set_data(sqe, NULL);
		io_uring_submit_now(data);
	} else {
		close(descriptor);
	}

	// We don't wait for the result of close since it has no use in pratice:
	return Qtrue;
}

static
struct __kernel_timespec * make_timeout(VALUE duration, struct __kernel_timespec *storage) {
	if (duration == Qnil) {
		return NULL;
	}
	
	if (FIXNUM_P(duration)) {
		storage->tv_sec = NUM2TIMET(duration);
		storage->tv_nsec = 0;
		
		return storage;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		time_t seconds = value;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

static
int timeout_nonblocking(struct __kernel_timespec *timespec) {
	return timespec && timespec->tv_sec == 0 && timespec->tv_nsec == 0;
}

struct select_arguments {
	struct IO_Event_Selector_URing *data;
	
	int result;
	
	struct __kernel_timespec storage;
	struct __kernel_timespec *timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	struct io_uring_cqe *cqe = NULL;
	
	arguments->result = io_uring_wait_cqe_timeout(&arguments->data->ring, &cqe, arguments->timeout);
	
	return NULL;
}

static
int select_internal_without_gvl(struct select_arguments *arguments) {
	io_uring_submit_flush(arguments->data);
	
	arguments->data->blocked = 1;
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->data->blocked = 0;
	
	if (arguments->result == -ETIME) {
		arguments->result = 0;
	} else if (arguments->result < 0) {
		rb_syserr_fail(-arguments->result, "select_internal_without_gvl:io_uring_wait_cqes");
	} else {
		// At least 1 event is waiting:
		arguments->result = 1;
	}
	
	return arguments->result;
}

// #define IO_EVENT_SELECTOR_URING_UDATA_INTERRUPT ((__u64) -2)

static inline
unsigned select_process_completions(struct io_uring *ring) {
	unsigned completed = 0;
	unsigned head;
	struct io_uring_cqe *cqe;
	
	io_uring_for_each_cqe(ring, head, cqe) {
		++completed;
		
		// If the operation was cancelled, or the operation has no user data (fiber):
		if (cqe->res == -ECANCELED || cqe->user_data == 0 || cqe->user_data == LIBURING_UDATA_TIMEOUT) {
			io_uring_cq_advance(ring, 1);
			continue;
		}
		
		// if (cqe->user_data == IO_EVENT_SELECTOR_URING_UDATA_INTERRUPT) {
		// 	io_uring_cq_advance(ring, 1);
		// }
		
		VALUE fiber = (VALUE)cqe->user_data;
		VALUE result = RB_INT2NUM(cqe->res);
		
		if (DEBUG) fprintf(stderr, "cqe res=%d user_data=%p\n", cqe->res, (void*)cqe->user_data);
		
		io_uring_cq_advance(ring, 1);
		
		IO_Event_Selector_fiber_transfer(fiber, 1, &result);
	}
	
	// io_uring_cq_advance(ring, completed);
	
	if (DEBUG) fprintf(stderr, "select_process_completions(completed=%d)\n", completed);
	
	return completed;
}

VALUE IO_Event_Selector_URing_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	int ready = IO_Event_Selector_queue_flush(&data->backend);
	
	int result = select_process_completions(&data->ring);
	
	// If the ready list was empty, we didn't process any completions, and the ready list is still empty:
	if (!ready && result == 0 && !data->backend.ready) {
		// We might need to wait for events:
		struct select_arguments arguments = {
			.data = data,
			.timeout = NULL,
		};
		
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!data->backend.ready && !timeout_nonblocking(arguments.timeout)) {
			// This is a blocking operation, we wait for events:
			result = select_internal_without_gvl(&arguments);
		} else {
			// The timeout specified required "nonblocking" behaviour so we just flush the SQ if required:
			io_uring_submit_flush(data);
		}
		
		// After waiting/flushing the SQ, check if there are any completions:
		result = select_process_completions(&data->ring);
	}
	
	return RB_INT2NUM(result);
}

VALUE IO_Event_Selector_URing_wakeup(VALUE self) {
	struct IO_Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_URing, &IO_Event_Selector_URing_Type, data);
	
	// If we are blocking, we can schedule a nop event to wake up the selector:
	if (data->blocked) {
		struct io_uring_sqe *sqe = NULL;
		
		while (true) {
			sqe = io_uring_get_sqe(&data->ring);
			if (sqe) break;
			
			rb_thread_schedule();
			
			// It's possible we became unblocked already, so we can assume the selector has already cycled at least once:
			if (!data->blocked) return Qfalse;
		}
		
		io_uring_prep_nop(sqe);
		io_uring_submit(&data->ring);
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_URing(VALUE IO_Event_Selector) {
	IO_Event_Selector_URing = rb_define_class_under(IO_Event_Selector, "URing", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_URing);
	
	rb_define_alloc_func(IO_Event_Selector_URing, IO_Event_Selector_URing_allocate);
	rb_define_method(IO_Event_Selector_URing, "initialize", IO_Event_Selector_URing_initialize, 1);
	
	rb_define_method(IO_Event_Selector_URing, "loop", IO_Event_Selector_URing_loop, 0);
	
	rb_define_method(IO_Event_Selector_URing, "transfer", IO_Event_Selector_URing_transfer, 0);
	rb_define_method(IO_Event_Selector_URing, "resume", IO_Event_Selector_URing_resume, -1);
	rb_define_method(IO_Event_Selector_URing, "yield", IO_Event_Selector_URing_yield, 0);
	rb_define_method(IO_Event_Selector_URing, "push", IO_Event_Selector_URing_push, 1);
	rb_define_method(IO_Event_Selector_URing, "raise", IO_Event_Selector_URing_raise, -1);
	
	rb_define_method(IO_Event_Selector_URing, "ready?", IO_Event_Selector_URing_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_URing, "select", IO_Event_Selector_URing_select, 1);
	rb_define_method(IO_Event_Selector_URing, "wakeup", IO_Event_Selector_URing_wakeup, 0);
	rb_define_method(IO_Event_Selector_URing, "close", IO_Event_Selector_URing_close, 0);
	
	rb_define_method(IO_Event_Selector_URing, "io_wait", IO_Event_Selector_URing_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_URing, "io_read", IO_Event_Selector_URing_io_read, 4);
	rb_define_method(IO_Event_Selector_URing, "io_write", IO_Event_Selector_URing_io_write, 4);
#endif
	
	rb_define_method(IO_Event_Selector_URing, "io_close", IO_Event_Selector_URing_io_close, 1);
	
	rb_define_method(IO_Event_Selector_URing, "process_wait", IO_Event_Selector_URing_process_wait, 3);
}
