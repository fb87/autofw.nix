import 'package:flutter/material.dart';

import '../widgets/section_card.dart';

/// Form & input controls: text fields, checkbox, radio, switch, slider,
/// dropdown, plus a validation demo.
class FormPanel extends StatefulWidget {
  const FormPanel({super.key});

  @override
  State<FormPanel> createState() => _FormPanelState();
}

class _FormPanelState extends State<FormPanel> {
  final _controller = TextEditingController(text: 'hello');
  final _formKey = GlobalKey<FormState>();

  bool _checked = true;
  bool _remember = false;
  String _radio = 'b';
  double _slider = 40;
  double _volume = 60;
  String _dropdown = 'dart';
  String _confirm = '';

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        SectionCard(
          title: 'Text fields',
          children: [
            TextField(
              controller: _controller,
              decoration: const InputDecoration(
                labelText: 'Basic text field',
                hintText: 'Type something…',
                prefixIcon: Icon(Icons.edit),
                border: OutlineInputBorder(),
              ),
              onChanged: (v) => setState(() => _confirm = v),
            ),
            const SizedBox(height: 12),
            TextField(
              obscureText: true,
              decoration: const InputDecoration(
                labelText: 'Password (obscured)',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            const TextField(
              maxLines: 3,
              decoration: InputDecoration(
                labelText: 'Multiline',
                alignLabelWithHint: true,
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            Text('You typed: $_confirm'),
          ],
        ),
        SectionCard(
          title: 'Validated form',
          children: [
            Form(
              key: _formKey,
              child: TextFormField(
                decoration: const InputDecoration(
                  labelText: 'Required field',
                  helperText: 'Submit to validate',
                  border: OutlineInputBorder(),
                ),
                validator: (v) =>
                    (v == null || v.isEmpty) ? 'Cannot be empty' : null,
              ),
            ),
            const SizedBox(height: 12),
            FilledButton(
              onPressed: () {
                setState(() => _formKey.currentState!.validate());
              },
              child: const Text('Validate'),
            ),
          ],
        ),
        SectionCard(
          title: 'Boolean inputs',
          children: [
            CheckboxListTile(
              title: const Text('Checkbox'),
              value: _checked,
              onChanged: (v) => setState(() => _checked = v ?? false),
            ),
            SwitchListTile(
              title: const Text('Switch'),
              subtitle: const Text('Remember me'),
              value: _remember,
              onChanged: (v) => setState(() => _remember = v),
            ),
            RadioGroup<String>(
              groupValue: _radio,
              onChanged: (v) => setState(() => _radio = v ?? 'b'),
              child: const Row(
                children: [
                  Text('Radio: '),
                  Radio(value: 'a'),
                  Text('A'),
                  Radio(value: 'b'),
                  Text('B'),
                ],
              ),
            ),
            Row(
              children: [
                const Icon(Icons.volume_down),
                Expanded(
                  child: Slider(
                    value: _volume,
                    max: 100,
                    divisions: 10,
                    label: _volume.round().toString(),
                    onChanged: (v) => setState(() => _volume = v),
                  ),
                ),
                Icon(_volume > 66 ? Icons.volume_up : Icons.volume_up),
                Text(' ${_volume.round()}'),
              ],
            ),
            DropdownButtonFormField<String>(
              initialValue: _dropdown,
              decoration: const InputDecoration(labelText: 'Language'),
              items: const [
                DropdownMenuItem(value: 'dart', child: Text('Dart')),
                DropdownMenuItem(value: 'cpp', child: Text('C++')),
                DropdownMenuItem(value: 'rust', child: Text('Rust')),
              ],
              onChanged: (v) => setState(() => _dropdown = v ?? 'dart'),
            ),
            Slider(
              value: _slider,
              max: 100,
              onChanged: (v) => setState(() => _slider = v),
            ),
            Text('Slider value: ${_slider.round()}'),
          ],
        ),
      ],
    );
  }
}
