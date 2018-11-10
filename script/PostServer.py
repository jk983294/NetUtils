#!/opt/anaconda3/bin/python
import multiprocessing
from socketserver import ThreadingMixIn
from http.server import BaseHTTPRequestHandler, HTTPServer
import logging
import json
import gzip
from io import BytesIO
import util as ptsutil
import datetime
import time


async_result = {}


def gzip_encode(content):
    out = BytesIO()
    f = gzip.GzipFile(fileobj=out, mode='wb', compresslevel=5)
    f.write(content)
    f.close()
    return out.getvalue()


def worker_callback(result):
    print("going to put result", result)
    ticket = result['ticket']
    async_result[ticket] = result


def worker_job(ticket, startTS):
    print("handling ticket ", ticket)
    time.sleep(10)
    print("going to put result of", ticket)
    return {'ticket': ticket, 'status': True, 'result': "result of %s" % ticket,
            'finishTS': datetime.datetime.now().strftime('%Y%m%d-%H:%M:%S'), 'startTS': startTS}


class PostServerHandler(BaseHTTPRequestHandler):
    worker_pool = multiprocessing.Pool(3)

    def log_message(self, format, *args):
        try:
            log_str = format % args
            self.server.logger.debug(log_str)
        except Exception as e:
            pass

    def _set_response(self, gzip=False, length=0):
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        if length != 0:
            self.send_header('Content-length', str(length))
        if gzip:
            self.send_header('Content-Encoding', 'gzip')
        self.end_headers()

    def print_head(self):
        return 'GET request,\nPath: %s\nHeaders:\n%s\n' % (str(self.path), str(self.headers))

    def do_GET(self):
        self._set_response()
        self.wfile.write("GET request for {}".format(self.path).encode('utf-8'))

    def do_POST(self):
        global async_result
        if self.path[0] != '/':
            self.path = '/' + self.path
        path_list = self.path.split('/')

        content_length = int(self.headers['Content-Length'])
        post_data = json.loads(self.rfile.read(content_length).decode('utf-8'))
        print('do_POST', path_list, post_data)
        if path_list[1] == 'query_ticket':
            ticket = post_data['ticket']
            if ticket not in async_result:
                self._set_response(gzip=False)
                self.wfile.write(json.dumps(
                    {'status': False, 'reason': 'ticket %s not exists' % ticket}).encode('utf-8'))
            else:
                self._set_response(gzip=True)
                self.wfile.write(gzip_encode(json.dumps({'status': True, 'data': async_result[ticket]}).encode('utf-8')))
        elif path_list[1] == 'async_query':
            ticket = ptsutil.generate_random_string(5)
            startTS = datetime.datetime.now().strftime('%Y%m%d-%H:%M:%S')
            async_result[ticket] = {'id': ticket, 'status': 'enqueue', 'startTS': startTS}
            self.worker_pool.apply_async(worker_job, args=(ticket, startTS), callback=worker_callback)
            self._set_response(gzip=False)
            self.wfile.write(json.dumps({'status': True, 'ticket': '%s' % ticket}).encode('utf-8'))
        elif path_list[1] == 'test_empty_response':
            print('recv test_empty_response')
            pass
        else:
            ret_value = {"echo": post_data}
            self._set_response(gzip=True)
            self.wfile.write(gzip_encode(json.dumps(ret_value).encode('utf-8')))


class ThreadingSimpleServer(ThreadingMixIn, HTTPServer):
    def set_logger(self, logger):
        self.logger = logger


class PostServer(object):
    def __init__(self, addr, port, handler, logger):
        self.logger = logger
        self.addr = addr
        self.port = port
        self.handler = handler

    def run(self):
        server_address = (self.addr, self.port)
        httpd = ThreadingSimpleServer(server_address, self.handler)
        httpd.set_logger(self.logger)
        self.logger.info('Starting post server at %s:%s...' % (self.addr, str(self.port)))
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
        httpd.server_close()
        self.logger.info('Stopping httpd...')


if __name__ == '__main__':
    from sys import argv
    logging.basicConfig(level=logging.INFO)
    if len(argv) == 2:
        server = PostServer('', int(argv[1]), PostServerHandler, logging)
    else:
        server = PostServer('', 18123, PostServerHandler, logging)
    server.run()
