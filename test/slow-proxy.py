#!/usr/bin/env python

import argparse
import errno
import signal
import sys
import socket
import threading
import datetime
import time
from random import random

run = True


def log(msg):
    dt = datetime.datetime.now()
    s = dt.strftime("%Y-%m-%d %H:%M:%S")
    sys.stdout.write("[%s] %s\n" % (s, msg))
    sys.stdout.flush()


def parse_cmdline():
    parser = argparse.ArgumentParser(description='Slow network receiver')

    parser.add_argument('-i',
                        '--ip',
                        dest='ip',
                        action='store',
                        type=str,
                        default='127.0.0.1',
                        help='listen ip')

    parser.add_argument('-p',
                        '--port',
                        dest='port',
                        action='store',
                        type=int,
                        required=True,
                        help='listen port')

    parser.add_argument('-F',
                        '--forward-host',
                        dest='forward_host',
                        action='store',
                        type=str,
                        default='127.0.0.1',
                        help='forward to host')

    parser.add_argument('-f',
                        '--forward',
                        dest='forward',
                        action='store',
                        type=int,
                        required=True,
                        help='forward to port')

    parser.add_argument('-m',
                        '--min-delay',
                        dest='min_delay',
                        action='store',
                        type=int,
                        default=0,
                        help='min delay')

    parser.add_argument('-x',
                        '--max-delay',
                        dest='max_delay',
                        action='store',
                        type=int,
                        default=0,
                        help='max delay')

    parser.add_argument('-v',
                        '--verbose',
                        dest='verbose',
                        action='store_true',
                        default=False,
                        help='verbose')

    return parser.parse_args()


def rand_int(min, max):
    return int(min + (max - min) * random())


def exit_gracefully(signum, frame):
    global run
    run = False


def handle_client_connection(address, client_sock, forward_host, forward_port,
                             min_delay, max_delay, verbose):
    try:
        client_sock.settimeout(15)
        forward_sock = socket.socket()
        forward_sock.settimeout(10)
        forward_sock.connect((forward_host, forward_port))
        while run:
            request = client_sock.recv(4096)
            if request:
                forward_sock.send(request)
                if verbose:
                    log('write %d bytes' % len(request))
            else:
                break
            delay = rand_int(min_delay, max_delay)
            if verbose:
                log('delay %ds' % delay)
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    except Exception as e:
        if verbose:
            log('%s:%s: %s' % (address[0], address[1], str(e)))
    finally:
        client_sock.close()
        forward_sock.close()
        if verbose:
            log('%s:%s: close' % (address[0], address[1]))


args = parse_cmdline()
if args.min_delay < 0:
    raise ValueError("delay must be greater or equal 0")
if args.min_delay > args.max_delay:
    raise ValueError("max_delay < min_delay")

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((args.ip, args.port))
server.listen(50)  # max backlog of connections

if args.verbose:
    log('listening on %s:%s' % (args.ip, args.port))

signal.signal(signal.SIGINT, exit_gracefully)
signal.signal(signal.SIGTERM, exit_gracefully)

while run:
    try:
        client_sock, address = server.accept()
        if args.verbose:
            log('accepted connection from %s:%s' % (address[0], address[1]))

        p = threading.Thread(target=handle_client_connection,
                             args=(address, client_sock, args.forward_host,
                                   args.forward, args.min_delay,
                                   args.max_delay, args.verbose))
        p.daemon = True
        p.start()
    except KeyboardInterrupt:
        run = False
    except socket.error as (code, msg):
        if code != errno.EINTR:
            log(msg)

sys.exit(0)
