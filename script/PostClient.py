#!/opt/anaconda3/bin/python
import requests
from requests.adapters import HTTPAdapter
from requests.packages.urllib3.util.retry import Retry
import traceback
import sys
import time
import json


class PostClient(object):
    def __init__(self, addr=None, port=None):
        if addr is None:
            raise ValueError('cannot find address %s' % addr)
        self.port = int(port)
        self.addr_line = 'http://%s:%d' % (addr, port)
        requests.adapters.DEFAULT_RETRIES = 3

    def post(self, prefix, payload, jsondata=None, timeout=5):
        s = requests.session()
        s.keep_alive = False
        retry = Retry(connect=3, backoff_factor=0.5)
        adapter = HTTPAdapter(max_retries=retry)
        s.mount('http://', adapter)
        s.mount('https://', adapter)

        if len(prefix) != 0 and prefix[0] != '/':
            prefix = '/' + prefix
        if jsondata is not None:
            jsondata['type'] = payload['type']
            try:
                ret = s.post(self.addr_line + prefix, json=jsondata, timeout=timeout)
            except Exception as e:
                traceback.print_exception(*sys.exc_info())
                return {'status': False, 'code': 400, 'reason': str(e)}
        else:
            jsondata = {'type': payload['type']}
            try:
                ret = s.post(self.addr_line + prefix, json=jsondata, timeout=timeout)
            except Exception as e:
                return {'status': False, 'code': 400, 'reason': str(e)}
        if ret.reason != 'OK':
            return {'status': False, 'code': ret.status_code, 'reason': ret.reason}
        if ret.text.strip() == '':
            return {'status': True, 'data': {}}
        return ret.json()

    def _query_by_ticket(self, ticket):
        return self.post('query_ticket', {'type': 'query'}, {'ticket': ticket}, timeout=5)

    def query_by_ticket(self, async_result):
        if async_result and 'ticket' in async_result:
            ticket = async_result['ticket']
            while True:
                result = self._query_by_ticket(ticket)
                print('query_by_ticket', result)
                if not result['status'] or 'data' not in result:
                    print('something wrong:', result['reason'], file=sys.stderr)
                    return None
                elif result['data']['status'] == 'enqueue':
                    time.sleep(2)
                elif result['data']['status'] == True:
                    return result['data']['result']
                else:
                    print('something wrong:', result, file=sys.stderr)
                    return None
        else:
            return None

    def async_query(self, data):
        return self.post('async_query', {'type': 'query'}, {'data': data}, timeout=5)


def test(port):
    client = PostClient('localhost', port)
    result = client.post('/test', {'type': 'query'}, {'args': "arg_test"})
    print(result)
    result = client.async_query('test')
    print(client.query_by_ticket(result))


if __name__ == '__main__':
    from sys import argv
    if len(argv) == 5:
        host, port, path, args_json = argv[1], int(argv[2]), argv[3], argv[4]
        if not path.startswith('/'):
            path = '/' + path
        client = PostClient(host, port)
        result = client.post(path, {'type': 'query'}, json.loads(args_json))
        print(result)
    elif len(argv) == 1:
        test(18180)
    elif len(argv) == 2:
        test(int(argv[1]))
    else:
        print("./PostClient.py <host> <port> <path> <args_json>")
