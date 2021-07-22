// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2021 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 */

#include "iio.h"
#include "iio-backend.h"
#include "iio-lock.h"
#include "iiod-responder.h"

#include <errno.h>
#include <string.h>

struct iiod_client_data {
	/*
	 * Structure for the command to send.
	 * When reading response, return code will be stored in the code field.
	 */
	struct iiod_command cmd;

	/* User-provided buffer where the data will be written */
	const struct iiod_buf *buf;
	size_t nb_buf;

	iiod_async_cleanup_t cleanup_cb;
	void *cleanup_cb_d;
};

struct iiod_io {
	struct iiod_io *r_next, *w_next;
	uint16_t client_id;

	struct iiod_responder *responder;

	/* Cond to sleep until I/O is done */
	struct iio_cond *cond;

	/* Set to true when the response has been read */
	bool r_done;

	/* Set to true when the command has been written */
	bool w_done;

	/* I/O data */
	struct iiod_client_data w_io, r_io;
};

struct iiod_responder {
	const struct iiod_responder_ops *ops;
	void *d;

	struct iiod_io *readers;
	uint16_t next_client_id;

	struct iio_mutex *lock, *rlock, *wlock;
	struct iio_thrd *read_thrd;
	struct iio_thrd *write_thrd;

	struct iiod_io *writers;
	struct iio_cond *wcond;

	bool thrd_stop;
};

static void __iiod_io_cancel_unlocked(struct iiod_io *io)
{
	struct iiod_responder *priv = io->responder;
	struct iiod_io *tmp;

	/* Discard the entry from the readers list */
	if (io == priv->readers) {
		priv->readers = NULL;
	} else if (priv->readers) {
		for (tmp = priv->readers; tmp->r_next; tmp = tmp->r_next) {
			if (tmp->r_next == io) {
				tmp->r_next = io->r_next;
				break;
			}
		}
	}
}

static void __iiod_writer_cancel_unlocked(struct iiod_io *io)
{
	struct iiod_responder *priv = io->responder;
	struct iiod_io *tmp;

	/* Discard the entry from the writers list */
	if (io == priv->writers) {
		priv->writers = NULL;
	} else if (priv->writers) {
		for (tmp = priv->writers; tmp->w_next; tmp = tmp->w_next) {
			if (tmp->w_next == io) {
				tmp->w_next = io->w_next;
				break;
			}
		}
	}
}

static ssize_t iiod_rw_all(struct iiod_responder *priv,
			   const struct iiod_buf *cmd_buf,
			   const struct iiod_buf *buf, size_t nb,
			   size_t bytes, bool is_read)
{
	ssize_t ret, count = 0;
	struct iiod_buf bufs[32], *curr = &bufs[0];

	if (cmd_buf)
		nb++;

	if (nb == 0 || nb > ARRAY_SIZE(bufs))
		return EINVAL;

	if (cmd_buf) {
		bufs[0] = *cmd_buf;
		if (buf)
			memcpy(&bufs[1], buf, (nb - 1) * sizeof(*buf));
	} else {
		memcpy(bufs, buf, nb * sizeof(*buf));
	}

	while (true) {
		if (is_read && bytes - count <= curr->size) {
			curr->size = bytes - count;
			nb = 1;
		}

		if (is_read)
			ret = priv->ops->read(priv->d, curr, nb);
		else
			ret = priv->ops->write(priv->d, curr, nb);
		if (ret <= 0)
			return ret;

		while (ret && (size_t) ret >= curr->size) {
			ret -= curr->size;
			count += curr->size;
			nb--;
			curr++;
		}

		if (!ret && !nb)
			break;

		count += ret;
		curr->ptr = (char *) curr->ptr + ret;
		curr->size -= ret;
	}

	return count;
}

static int iiod_discard_data(struct iiod_responder *priv, size_t bytes)
{
	ssize_t ret;

	iio_mutex_lock(priv->rlock);
	while (bytes) {
		ret = priv->ops->discard(priv->d, bytes);
		if (ret < 0) {
			iio_mutex_unlock(priv->rlock);
			return ret;
		}

		bytes -= (size_t) ret;
	}

	iio_mutex_unlock(priv->rlock);

	return 0;
}

ssize_t iiod_command_data_read(struct iiod_command_data *data,
			       const struct iiod_buf *buf)
{
	struct iiod_responder *priv = (struct iiod_responder *) data;

	return iiod_rw_all(priv, NULL, buf, 1, buf->size, true);
}

static ssize_t iiod_run_command(struct iiod_responder *priv,
				struct iiod_command *cmd)
{
	return priv->ops->cmd(cmd, (struct iiod_command_data *) priv, priv->d);
}

static int iiod_responder_reader_thrd(void *d)
{
	struct iiod_responder *priv = d;
	struct iiod_command cmd;
	struct iiod_buf cmd_buf;
	struct iiod_io *io;
	ssize_t ret = 0;

	cmd_buf.ptr = &cmd;
	cmd_buf.size = sizeof(cmd);

	while (!priv->thrd_stop) {
		ret = iiod_rw_all(priv, NULL, &cmd_buf, 1, sizeof(cmd), true);
		if (ret <= 0) {
			printf("iiod_rw_all returned %zd\n", ret);
			break;
		}

		if (cmd.op != IIOD_OP_RESPONSE) {
			ret = iiod_run_command(priv, &cmd);
			if (ret < 0)
				break;

			continue;
		}

		iio_mutex_lock(priv->rlock);

		/* Find the client for the given ID in the readers list */
		for (io = priv->readers; io; io = io->r_next) {
			if (io->client_id == cmd.client_id)
				break;
		}

		if (!io) {
			/* We received a response, but have no client waiting
			 * for it, so drop it. */
			iio_mutex_unlock(priv->rlock);

			iiod_discard_data(priv, cmd.code);
			continue;
		}

		/* Discard the entry from the readers list */
		__iiod_io_cancel_unlocked(io);

		iio_mutex_unlock(priv->rlock);

		io->r_io.cmd.code = cmd.code;

		if (io->r_io.buf && cmd.code > 0) {
			ret = iiod_rw_all(priv, NULL, io->r_io.buf,
					  io->r_io.nb_buf, cmd.code, true);

			if (ret > 0 && (size_t) ret < (size_t) cmd.code)
				iiod_discard_data(priv, cmd.code - ret);
		}

		/* Wake up the reader */
		iio_mutex_lock(priv->rlock);
		io->r_done = true;
		iio_cond_signal(io->cond);
		iio_mutex_unlock(priv->rlock);

		if (io->r_io.nb_buf && io->r_io.cleanup_cb)
			io->r_io.cleanup_cb(io->r_io.cleanup_cb_d, ret);
	}

	priv->thrd_stop = true;

	return ret;
}

static int iiod_responder_writer_thrd(void *d)
{
	struct iiod_responder *priv = d;
	struct iiod_io *writer;
	struct iiod_command cmd;
	struct iiod_buf cmd_buf;
	unsigned int i;
	ssize_t ret;

	cmd_buf.ptr = &cmd;
	cmd_buf.size = sizeof(cmd);

	iio_mutex_lock(priv->wlock);

	for (;;) {
		while (!priv->writers && !priv->thrd_stop)
			iio_cond_wait(priv->wcond, priv->wlock);

		printf("Write thread awaken\n");

		if (priv->thrd_stop)
			break;

		writer = priv->writers;
		priv->writers = writer->w_next;
		iio_mutex_unlock(priv->wlock);

		cmd = writer->w_io.cmd;

		printf("Write all...\n");
		ret = iiod_rw_all(priv, &cmd_buf, writer->w_io.buf,
				  writer->w_io.nb_buf, 0, false);
		printf("Write all done\n");

		iio_mutex_lock(priv->wlock);
		writer->w_io.cmd.code = (intptr_t) ret;

		/* Wake up the writer */
		printf("Wake up writer\n");
		writer->w_done = true;
		iio_cond_signal(writer->cond);

		if (writer->w_io.nb_buf && writer->w_io.cleanup_cb)
			writer->w_io.cleanup_cb(writer->w_io.cleanup_cb_d, ret);
	}

	printf("Write thread stopped!\n");
	iio_mutex_unlock(priv->wlock);
}

static int iiod_enqueue_command(struct iiod_io *writer, uint8_t op,
				uint8_t dev, int32_t code,
				const struct iiod_buf *buf, size_t nb,
				iiod_async_cleanup_t cleanup_cb, void *d)
{
	struct iiod_responder *priv = writer->responder;
	struct iiod_io *last;
	unsigned int i;
	int ret;

	writer->w_io.cmd.op = op;
	writer->w_io.cmd.dev = dev;
	writer->w_io.cmd.client_id = writer->client_id;
	writer->w_io.cmd.code = code;
	writer->w_io.buf = buf;
	writer->w_io.nb_buf = nb;
	writer->w_io.cleanup_cb = cleanup_cb;
	writer->w_io.cleanup_cb_d = d;
	writer->w_next = NULL;
	writer->w_done = false;

	iio_mutex_lock(priv->wlock);

	if (!priv->writers) {
		priv->writers = writer;
	} else {
		for (last = priv->writers; last->w_next; )
			last = last->w_next;

		last->w_next = writer;
	}

	iio_cond_signal(priv->wcond);
	iio_mutex_unlock(priv->wlock);

	return 0;
}

void iiod_io_wait_for_command_done(struct iiod_io *io)
{
	struct iiod_responder *priv = io->responder;

	iio_mutex_lock(priv->wlock);

	while (!io->w_done)
		iio_cond_wait(io->cond, priv->wlock);

	io->w_done = false;
	iio_mutex_unlock(priv->wlock);
}

static void wait_for_response(struct iiod_io *io, bool unlock)
{
	struct iiod_responder *priv = io->responder;

	iio_mutex_lock(priv->rlock);

	while (!io->r_done)
		iio_cond_wait(io->cond, priv->rlock);

	io->r_done = false;
	if (unlock)
		iio_mutex_unlock(priv->rlock);
}

intptr_t iiod_io_wait_for_response(struct iiod_io *io)
{
	wait_for_response(io, true);

	return io->r_io.cmd.code;
}

int iiod_io_send_command_async(struct iiod_io *io,
			       const struct iiod_command *cmd,
			       const struct iiod_buf *buf, size_t nb,
			       iiod_async_cleanup_t cleanup_cb, void *d)
{
	return iiod_enqueue_command(io, cmd->op, cmd->dev,
				    cmd->code, buf, nb, cleanup_cb, d);
}

int iiod_io_send_command(struct iiod_io *io,
			 const struct iiod_command *cmd,
			 const struct iiod_buf *buf, size_t nb)
{
	int ret;

	ret = iiod_io_send_command_async(io, cmd, buf, nb, NULL, NULL);
	if (ret)
		return ret;

	iiod_io_wait_for_command_done(io);

	return 0;
}

int iiod_io_send_response_async(struct iiod_io *io, intptr_t code,
				const struct iiod_buf *buf, size_t nb,
				iiod_async_cleanup_t cleanup_cb, void *d)
{
	return iiod_enqueue_command(io, IIOD_OP_RESPONSE, 0, code, buf, nb,
				    cleanup_cb, d);
}

int iiod_io_send_response(struct iiod_io *io, intptr_t code,
			  const struct iiod_buf *buf, size_t nb)
{
	int ret;

	ret = iiod_io_send_response_async(io, code, buf, nb, NULL, NULL);
	if (ret)
		return ret;

	iiod_io_wait_for_command_done(io);

	return 0;
}

static void iiod_io_enqueue_response_request(struct iiod_io *io,
					     const struct iiod_buf *buf,
					     size_t nb, bool lock)
{
	struct iiod_responder *priv = io->responder;
	struct iiod_io *tmp;

	if (lock)
		iio_mutex_lock(priv->rlock);

	io->r_io.buf = buf;
	io->r_io.nb_buf = nb;
	io->r_io.cleanup_cb = NULL;
	io->r_done = false;

	/* Add it to the readers list */
	if (!priv->readers) {
		priv->readers = io;
	} else {
		for (tmp = priv->readers; tmp->r_next; )
			tmp = tmp->r_next;
		tmp->r_next = io;
	}

	iio_mutex_unlock(priv->rlock);
}

static intptr_t iiod_io_read_response(struct iiod_io *io, bool unlock)
{
	wait_for_response(io, unlock);

	return io->r_io.cmd.code;
}

void iiod_io_get_response_async(struct iiod_io *io,
				const struct iiod_buf *buf, size_t nb)
{
	iiod_io_enqueue_response_request(io, buf, nb, true);
}

intptr_t iiod_io_get_response(struct iiod_io *io,
			      const struct iiod_buf *buf, size_t nb)
{
	iiod_io_get_response_async(io, buf, nb);

	return iiod_io_wait_for_response(io);
}

intptr_t iiod_io_get_and_request_response(struct iiod_io *io,
					  const struct iiod_buf *buf, size_t nb)
{
	intptr_t ret = iiod_io_read_response(io, false);

	iiod_io_enqueue_response_request(io, buf, nb, false);

	return ret;
}

int iiod_io_exec_command(struct iiod_io *io,
			 const struct iiod_command *cmd,
			 const struct iiod_buf *cmd_buf,
			 const struct iiod_buf *buf)
{
	int ret;

	iiod_io_get_response_async(io, buf, buf != NULL);

	ret = iiod_io_send_command(io, cmd, cmd_buf, cmd_buf != NULL);
	if (ret < 0) {
		iiod_io_cancel(io);
		return ret;
	}

	return (int) iiod_io_wait_for_response(io);
}

struct iiod_responder *
iiod_responder_create(const struct iiod_responder_ops *ops, void *d)
{
	struct iiod_responder *priv;

	priv = zalloc(sizeof(*priv));
	if (!priv)
		return NULL;

	priv->ops = ops;
	priv->d = d;

	priv->lock = iio_mutex_create();
	if (!priv->lock)
		goto err_free_priv;

	priv->rlock = iio_mutex_create();
	if (!priv->rlock)
		goto err_free_lock;

	priv->wlock = iio_mutex_create();
	if (!priv->wlock)
		goto err_free_rlock;

	priv->wcond = iio_cond_create();
	if (!priv->wcond)
		goto err_free_wlock;

	priv->read_thrd = iio_thrd_create(iiod_responder_reader_thrd, priv,
					  "iiod-responder-reader-thd");
	if (IS_ERR(priv->read_thrd))
		goto err_free_wcond;

	priv->write_thrd = iio_thrd_create(iiod_responder_writer_thrd, priv,
					   "iiod-responder-writer-thd");
	if (IS_ERR(priv->read_thrd))
		goto err_free_read_thrd;

	return priv;

err_free_read_thrd:
	priv->thrd_stop = true;
	iiod_responder_wait_done(priv);
err_free_wcond:
	iio_cond_destroy(priv->wcond);
err_free_wlock:
	iio_mutex_destroy(priv->wlock);
err_free_rlock:
	iio_mutex_destroy(priv->rlock);
err_free_lock:
	iio_mutex_destroy(priv->lock);
err_free_priv:
	free(priv);
	return NULL;
}

void iiod_responder_destroy(struct iiod_responder *priv)
{
	priv->thrd_stop = true;
	iiod_responder_wait_done(priv);

	iio_cond_destroy(priv->wcond);
	iio_mutex_destroy(priv->wlock);
	iio_mutex_destroy(priv->rlock);
	iio_mutex_destroy(priv->lock);
	free(priv);
}

void iiod_responder_wait_done(struct iiod_responder *priv)
{
	if (priv->read_thrd) {
		/* TODO: check priv->thrd_stop */
		iio_thrd_join_and_destroy(priv->read_thrd);
		priv->read_thrd = NULL;
	}

	if (priv->write_thrd) {
		/* If the read thread stopped, notify the write thread */
		iio_mutex_lock(priv->wlock);
		priv->thrd_stop = true;
		iio_cond_signal(priv->wcond);
		iio_mutex_unlock(priv->wlock);

		iio_thrd_join_and_destroy(priv->write_thrd);
		priv->write_thrd = NULL;
	}
}

static uint16_t iiod_responder_get_new_id(struct iiod_responder *priv)
{
	uint16_t id;

	/* No atomics in C99... */
	iio_mutex_lock(priv->lock);
	id = priv->next_client_id++;
	iio_mutex_unlock(priv->lock);

	return id;
}

static struct iiod_io *
iiod_responder_create_io_from_id(struct iiod_responder *priv, uint16_t id)
{
	struct iiod_io *io;

	io = zalloc(sizeof(*io));
	if (!io)
		return NULL;

	io->responder = priv;

	io->cond = iio_cond_create();
	if (!io->cond)
		goto err_free_io;

	io->client_id = id;

	return io;

err_free_io:
	free(io);

	return NULL;
}

struct iiod_io * iiod_command_create_io(const struct iiod_command *cmd,
					struct iiod_command_data *data)
{
	struct iiod_responder *priv = (struct iiod_responder *) data;

	return iiod_responder_create_io_from_id(priv, cmd->client_id);
}

struct iiod_io * iiod_responder_create_io(struct iiod_responder *priv)
{
	uint16_t id = iiod_responder_get_new_id(priv);

	return iiod_responder_create_io_from_id(priv, id);
}

void iiod_io_cancel(struct iiod_io *io)
{
	struct iiod_responder *priv = io->responder;

	iio_mutex_lock(priv->rlock);
	__iiod_io_cancel_unlocked(io);
	iio_mutex_unlock(priv->rlock);

	iio_mutex_lock(priv->rlock);
	__iiod_writer_cancel_unlocked(io);
	iio_mutex_unlock(priv->rlock);
}

void iiod_io_destroy(struct iiod_io *io)
{
	iio_cond_destroy(io->cond);
	free(io);
}