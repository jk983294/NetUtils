#!/opt/anaconda3/bin/python
import requests

parms = {
    'name1': 'value1',
    'name2': 'value2'
}

headers = {
    'User-agent': 'none/ofyourbusiness',
    'Spam': 'Eggs'
}

resp = requests.post('http://127.0.0.1:8081/', data=parms, headers=headers)

print(resp.content)         # binary data
print(resp.text)            # decoded data
print(resp.json)            # decoded json data
