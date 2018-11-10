#!/opt/anaconda3/bin/python
import requests

payload = {
    'name1': 'value1',
    'name2': 'value2'
}

#resp = requests.get('http://127.0.0.1:8080/', params=payload)
resp = requests.get('http://127.0.0.1:8081/')
#resp = requests.get('http://127.0.0.1:8080/index.hmtl')

print(resp.status_code)
# print(resp.headers['content-length'])
print(resp.text)
