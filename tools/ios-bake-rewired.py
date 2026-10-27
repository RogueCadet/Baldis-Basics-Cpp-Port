# Bake the iOS build's Rewired touch configuration into data/rewired-touch.json.
# Source: ios-ref/ios-rewired.json, the genuine level0 "Rewired Input Manager" dumped by
# tools/ios-dump-hierarchy.py (105/105-style exact reads). Everything here is copied, not chosen:
#   - actions (id, name),
#   - the TouchControl custom controller's elements (id, name, axis/button) and, per axis, its
#     HardwareAxisInfo._dataFormat (an AxisCoordinateMode: 0 Absolute, 1 Relative),
#   - Player0's custom-controller map: element -> action (one element may drive several actions:
#     the Interact button is mapped to BOTH Interact and Jump).
# usage: python3 tools/ios-bake-rewired.py
import json
import os

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = json.load(open(os.path.join(PORT, 'ios-ref', 'ios-rewired.json')))
u = [c for c in src['comps'] if c['script'] == 'Rewired.InputManager'][0]['fields']['_userData']
actions = {a['_id']: a['_name'] for a in u['actions']}
cc = [c for c in u['customControllers'] if c['_name'] == 'TouchControl'][0]
coord = {a['elementIdentifierId']: a['axisInfo']['_dataFormat'] for a in cc['_axes']}
elements = [{'id': e['_id'], 'name': e['_name'], 'axis': e['_elementType'] == 0,
             'coordinateMode': coord.get(e['_id'], 0)} for e in cc['_elementIdentifiers']]
maps = [m for m in u['customControllerMaps'] if m['categoryId'] == 0]
assert len(maps) == 1, 'expected one default-category custom controller map'
links = [{'element': a['_elementIdentifierId'], 'action': actions[a['_actionId']],
          'invert': bool(a.get('_invert', 0))} for a in maps[0]['actionElementMaps']]
out = {'source': 'genuine iOS 1.4.3 level0 Rewired Input Manager (TouchControl custom controller)',
       'actions': sorted(actions.values()), 'elements': elements, 'map': links}
json.dump(out, open(os.path.join(PORT, 'data', 'rewired-touch.json'), 'w'), indent=1)
print('rewired-touch.json: %d elements, %d element->action links' % (len(elements), len(links)))
