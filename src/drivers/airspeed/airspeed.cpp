/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file ets_airspeed.cpp
 * @author Simon Wilks
 *
 * Driver for the Eagle Tree Airspeed V3 connected via I2C.
 */

#include <nuttx/config.h>

#include <drivers/device/i2c.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <semaphore.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h>

#include <arch/board/board.h>

#include <systemlib/airspeed.h>
#include <systemlib/err.h>
#include <systemlib/param/param.h>
#include <systemlib/perf_counter.h>

#include <drivers/drv_airspeed.h>
#include <drivers/drv_hrt.h>

#include <uORB/uORB.h>
#include <uORB/topics/differential_pressure.h>
#include <uORB/topics/subsystem_info.h>

#include <drivers/airspeed/airspeed.h>

Airspeed::Airspeed(int bus, int address, unsigned conversion_interval) :
	I2C("Airspeed", AIRSPEED_DEVICE_PATH, bus, address, 100000),
	_num_reports(0),
	_next_report(0),
	_oldest_report(0),
	_reports(nullptr),
	_sensor_ok(false),
	_measure_ticks(0),
	_collect_phase(false),
	_diff_pres_offset(0.0f),
	_airspeed_pub(-1),
	_conversion_interval(conversion_interval),
	_sample_perf(perf_alloc(PC_ELAPSED, "airspeed_read")),
	_comms_errors(perf_alloc(PC_COUNT, "airspeed_comms_errors")),
	_buffer_overflows(perf_alloc(PC_COUNT, "airspeed_buffer_overflows"))
{
	// enable debug() calls
	_debug_enabled = true;

	// work_cancel in the dtor will explode if we don't do this...
	memset(&_work, 0, sizeof(_work));
}

Airspeed::~Airspeed()
{
	/* make sure we are truly inactive */
	stop();

	/* free any existing reports */
	if (_reports != nullptr)
		delete[] _reports;
}

int
Airspeed::init()
{
	int ret = ERROR;

	/* do I2C init (and probe) first */
	if (I2C::init() != OK)
		goto out;

	/* allocate basic report buffers */
	_num_reports = 2;
	_reports = new struct differential_pressure_s[_num_reports];

	for (unsigned i = 0; i < _num_reports; i++)
		_reports[i].max_differential_pressure_pa = 0;

	if (_reports == nullptr)
		goto out;

	_oldest_report = _next_report = 0;

	/* get a publish handle on the airspeed topic */
	memset(&_reports[0], 0, sizeof(_reports[0]));
	_airspeed_pub = orb_advertise(ORB_ID(differential_pressure), &_reports[0]);

	if (_airspeed_pub < 0)
		warnx("failed to create airspeed sensor object. Did you start uOrb?");

	ret = OK;
	/* sensor is ok, but we don't really know if it is within range */
	_sensor_ok = true;
out:
	return ret;
}

int
Airspeed::probe()
{
	return measure();
}

int
Airspeed::ioctl(struct file *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

	case SENSORIOCSPOLLRATE: {
			switch (arg) {

				/* switching to manual polling */
			case SENSOR_POLLRATE_MANUAL:
				stop();
				_measure_ticks = 0;
				return OK;

				/* external signalling (DRDY) not supported */
			case SENSOR_POLLRATE_EXTERNAL:

				/* zero would be bad */
			case 0:
				return -EINVAL;

				/* set default/max polling rate */
			case SENSOR_POLLRATE_MAX:
			case SENSOR_POLLRATE_DEFAULT: {
					/* do we need to start internal polling? */
					bool want_start = (_measure_ticks == 0);

					/* set interval for next measurement to minimum legal value */
					_measure_ticks = USEC2TICK(_conversion_interval);

					/* if we need to start the poll state machine, do it */
					if (want_start)
						start();

					return OK;
				}

				/* adjust to a legal polling interval in Hz */
			default: {
					/* do we need to start internal polling? */
					bool want_start = (_measure_ticks == 0);

					/* convert hz to tick interval via microseconds */
					unsigned ticks = USEC2TICK(1000000 / arg);

					/* check against maximum rate */
					if (ticks < USEC2TICK(_conversion_interval))
						return -EINVAL;

					/* update interval for next measurement */
					_measure_ticks = ticks;

					/* if we need to start the poll state machine, do it */
					if (want_start)
						start();

					return OK;
				}
			}
		}

	case SENSORIOCGPOLLRATE:
		if (_measure_ticks == 0)
			return SENSOR_POLLRATE_MANUAL;

		return (1000 / _measure_ticks);

	case SENSORIOCSQUEUEDEPTH: {
			/* add one to account for the sentinel in the ring */
			arg++;

			/* lower bound is mandatory, upper bound is a sanity check */
			if ((arg < 2) || (arg > 100))
				return -EINVAL;

			/* allocate new buffer */
			struct differential_pressure_s *buf = new struct differential_pressure_s[arg];

			if (nullptr == buf)
				return -ENOMEM;

			/* reset the measurement state machine with the new buffer, free the old */
			stop();
			delete[] _reports;
			_num_reports = arg;
			_reports = buf;
			start();

			return OK;
		}

	case SENSORIOCGQUEUEDEPTH:
		return _num_reports - 1;

	case SENSORIOCRESET:
		/* XXX implement this */
		return -EINVAL;

	case AIRSPEEDIOCSSCALE: {
		struct airspeed_scale *s = (struct airspeed_scale*)arg;
		_diff_pres_offset = s->offset_pa;
		return OK;
		}

	case AIRSPEEDIOCGSCALE: {
		struct airspeed_scale *s = (struct airspeed_scale*)arg;
		s->offset_pa = _diff_pres_offset;
		s->scale = 1.0f;
		return OK;
		}

	default:
		/* give it to the superclass */
		return I2C::ioctl(filp, cmd, arg);
	}
}

ssize_t
Airspeed::read(struct file *filp, char *buffer, size_t buflen)
{
	unsigned count = buflen / sizeof(struct differential_pressure_s);
	int ret = 0;

	/* buffer must be large enough */
	if (count < 1)
		return -ENOSPC;

	/* if automatic measurement is enabled */
	if (_measure_ticks > 0) {

		/*
		 * While there is space in the caller's buffer, and reports, copy them.
		 * Note that we may be pre-empted by the workq thread while we are doing this;
		 * we are careful to avoid racing with them.
		 */
		while (count--) {
			if (_oldest_report != _next_report) {
				memcpy(buffer, _reports + _oldest_report, sizeof(*_reports));
				ret += sizeof(_reports[0]);
				INCREMENT(_oldest_report, _num_reports);
			}
		}

		/* if there was no data, warn the caller */
		return ret ? ret : -EAGAIN;
	}

	/* manual measurement - run one conversion */
	/* XXX really it'd be nice to lock against other readers here */
	do {
		_oldest_report = _next_report = 0;

		/* trigger a measurement */
		if (OK != measure()) {
			ret = -EIO;
			break;
		}

		/* wait for it to complete */
		usleep(_conversion_interval);

		/* run the collection phase */
		if (OK != collect()) {
			ret = -EIO;
			break;
		}

		/* state machine will have generated a report, copy it out */
		memcpy(buffer, _reports, sizeof(*_reports));
		ret = sizeof(*_reports);

	} while (0);

	return ret;
}

void
Airspeed::start()
{
	/* reset the report ring and state machine */
	_collect_phase = false;
	_oldest_report = _next_report = 0;

	/* schedule a cycle to start things */
	work_queue(HPWORK, &_work, (worker_t)&Airspeed::cycle_trampoline, this, 1);

	/* notify about state change */
	struct subsystem_info_s info = {
		true,
		true,
		true,
		SUBSYSTEM_TYPE_DIFFPRESSURE
	};
	static orb_advert_t pub = -1;

	if (pub > 0) {
		orb_publish(ORB_ID(subsystem_info), pub, &info);

	} else {
		pub = orb_advertise(ORB_ID(subsystem_info), &info);
	}
}

void
Airspeed::stop()
{
	work_cancel(HPWORK, &_work);
}

void
Airspeed::cycle_trampoline(void *arg)
{
	Airspeed *dev = (Airspeed *)arg;

	dev->cycle();
}

void
Airspeed::print_info()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_comms_errors);
	perf_print_counter(_buffer_overflows);
	warnx("poll interval:  %u ticks", _measure_ticks);
	warnx("report queue:   %u (%u/%u @ %p)",
	       _num_reports, _oldest_report, _next_report, _reports);
}
