import 'package:flutter/material.dart';

import '../widgets/section_card.dart';

/// Selection primitives and list displays: ListTile, Card, Chip, DataTable,
/// ExpansionTile and a reorderable list.
class SelectionPanel extends StatefulWidget {
  const SelectionPanel({super.key});

  @override
  State<SelectionPanel> createState() => _SelectionPanelState();
}

class _SelectionPanelState extends State<SelectionPanel> {
  final List<String> _rows = ['Item one', 'Item two', 'Item three'];
  bool _phone = false;

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        SectionCard(title: 'ListTiles & cards', children: [
          const Card(
            child: ListTile(
              leading: CircleAvatar(child: Icon(Icons.person)),
              title: Text('A ListTile in a Card'),
              subtitle: Text('leading icon, title, subtitle'),
              trailing: Icon(Icons.chevron_right),
            ),
          ),
          const Card(
            color: Color(0x2033FF66),
            child: ListTile(
              leading: Icon(Icons.eco),
              title: Text('Custom card color'),
              subtitle: Text('Card + ListTile combined'),
            ),
          ),
          SwitchListTile(
            title: const Text('SwitchListTile'),
            secondary: const Icon(Icons.notifications),
            value: _phone,
            onChanged: (v) => setState(() => _phone = v),
          ),
        ]),
        SectionCard(title: 'Chips', children: [
          Wrap(spacing: 8, runSpacing: 8, children: [
            const Chip(label: Text('Chip'), avatar: Icon(Icons.star)),
            const ActionChip(label: Text('Action'), onPressed: null),
            InputChip(label: const Text('Input'), selected: true),
            const Chip(label: Text('Basic'), deleteIcon: Icon(Icons.close), onDeleted: null),
          ]),
        ]),
        SectionCard(title: 'DataTable', children: [
          DataTable(
            columns: const [
              DataColumn(label: Text('Name')),
              DataColumn(label: Text('Language'), numeric: true),
            ],
            rows: [
              DataRow(cells: const [DataCell(Text('Dart')), DataCell(Text('1'))]),
              DataRow(cells: const [DataCell(Text('C++')), DataCell(Text('2'))]),
              DataRow(cells: const [DataCell(Text('Rust')), DataCell(Text('3'))]),
            ],
          ),
        ]),
        SectionCard(title: 'ExpansionTile', children: const [
          ExpansionTile(
            leading: Icon(Icons.account_tree),
            title: Text('Expandable row'),
            children: [
              ListTile(title: Text('Nested detail one')),
              ListTile(title: Text('Nested detail two')),
            ],
          ),
        ]),
        SectionCard(title: 'ReorderableList', children: [
          ReorderableListView.builder(
            shrinkWrap: true,
            physics: const NeverScrollableScrollPhysics(),
            itemCount: _rows.length,
            onReorder: (a, b) {
              setState(() {
                if (b > a) b -= 1;
                final item = _rows.removeAt(a);
                _rows.insert(b, item);
              });
            },
            itemBuilder: (c, i) => ListTile(
              key: ValueKey(_rows[i]),
              leading: const Icon(Icons.drag_handle),
              title: Text(_rows[i]),
            ),
          ),
        ]),
      ],
    );
  }
}
