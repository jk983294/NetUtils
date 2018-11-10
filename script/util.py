#!/opt/anaconda3/bin/python
import random
import string
import xml.etree.ElementTree as etree
from pathlib import Path
from xml.dom import minidom


def prettify_xml(elem):
    rough_string = etree.tostring(elem).decode()
    rough_string = rough_string.replace('\n', ' ').replace('\r', '').replace('>\n', '>')
    reparsed = minidom.parseString(rough_string.encode())
    return '\n'.join([line for line in reparsed.toprettyxml(newl='\n', indent='     ').split('\n') if line.strip()])


def get_current_dir(filepath, level=0):
    return str(Path(filepath).resolve().parents[level])


def replace_special_timeholder(str, clock):
    str = str.replace('${YYYY}', '%04d' % clock.year)
    str = str.replace('${MM}', '%02d' % clock.month)
    str = str.replace('${DD}', '%02d' % clock.day)
    str = str.replace('${YYYYMMDD}', '%04d%02d%02d' % (clock.year, clock.month, clock.day))
    str = str.replace('${YYYYMM}', '%04d%02d' % (clock.year, clock.month))
    str = str.replace('${MMDD}', '%02d%02d' % (clock.month, clock.day))
    str = str.replace('${HHMMSS}', '%02d%02d%02d' % (clock.hour, clock.minute, clock.second))
    str = str.replace('${HHMMSSmmm}', '%02d%02d%02d%03d' %
                      (clock.hour, clock.minute, clock.second, clock.microsecond / 1000))
    return str


def replace_special_timeholder_copy(str, clock):
    newstr = str
    return replace_special_timeholder(newstr, clock)


def generate_random_string(length):
    return ''.join(random.choice(string.ascii_lowercase + string.ascii_uppercase + string.digits) for i in range(length))


if __name__ == '__main__':
    print(generate_random_string(4))
