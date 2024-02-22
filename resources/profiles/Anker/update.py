import json
from pathlib import Path
import sys


script_path = Path(__file__).resolve().parent
anker_json_path = script_path.parent / 'Anker.json'

if anker_json_path.exists():
    print(f'found: {anker_json_path}')
else:
    sys.exit()

anker_data = json.loads(anker_json_path.read_text())


machine_model_key = 'machine_model_list'
machine_key = 'machine_list'
process_key = 'process_list'
filament_key = 'filament_list'

machine_path = script_path / 'machine'
process_path = script_path / 'process'
filament_path = script_path / 'filament'

new_models = []
new_machines = []
new_processes = []
new_filaments = []

found_machines = {}

machine_paths = []

for entry in machine_path.iterdir():
    if entry.is_file() and entry.suffix.lower() == '.json':
        machine_paths.append(entry)

while machine_paths:
    working_machine_paths = machine_paths[:]
    machine_paths = []
    for entry in working_machine_paths:
        entry_data = json.loads(entry.read_text())
        if entry_data['type'] == 'machine':
            if 'inherits' not in entry_data or entry_data['inherits'] in found_machines:
                found_machines[entry_data['name']] = entry.relative_to(script_path)
                new_machines.append({'name': entry_data['name'], 'sub_path': str(entry.relative_to(script_path))})
            else:
                machine_paths.append(entry)
        elif entry_data['type'] == 'machine_model':
            new_models.append({'name': entry_data['name'], 'sub_path': str(entry.relative_to(script_path))})

process_paths = []
found_procs = {}

for entry in process_path.iterdir():
    if entry.is_file() and entry.suffix.lower() == '.json':
        process_paths.append(entry)

while process_paths:
    working_process_paths = process_paths[:]
    process_paths = []
    for entry in working_process_paths:
        entry_data = json.loads(entry.read_text())
        if entry_data['type'] == 'process':
            if 'inherits' not in entry_data or entry_data['inherits'] in found_procs:
                found_procs[entry_data['name']] = entry.relative_to(script_path)
                new_processes.append({'name': entry_data['name'], 'sub_path': str(entry.relative_to(script_path))})
            else:
                process_paths.append(entry)

filament_paths = []
found_fils = {}


for entry in filament_path.iterdir():
    if entry.is_file() and entry.suffix.lower() == '.json':
        filament_paths.append(entry)

while filament_paths:
    working_filament_paths = filament_paths[:]
    filament_paths = []
    for entry in working_filament_paths:
        entry_data = json.loads(entry.read_text())
        if entry_data['type'] == 'filament':
            if 'inherits' not in entry_data or entry_data['inherits'] in found_fils:
                found_fils[entry_data['name']] = entry.relative_to(script_path)
                new_filaments.append({'name': entry_data['name'], 'sub_path': str(entry.relative_to(script_path))})
            else:
                filament_paths.append(entry)

anker_data[machine_model_key] = new_models
anker_data[machine_key] = new_machines
anker_data[process_key] = new_processes
anker_data[filament_key] = new_filaments


with open(anker_json_path, 'w', encoding='utf-8') as f:
    json.dump(anker_data, f, ensure_ascii=False, indent=4)
