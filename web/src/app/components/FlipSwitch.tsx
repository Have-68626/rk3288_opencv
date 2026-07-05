import { Switch, Space, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: string;
  checked: boolean;
  disabled: boolean;
  onChange: (checked: boolean) => void;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({ label, checked, disabled, onChange }) => (
  <Space>
    <Text>{label}</Text>
    <Switch checked={checked} disabled={disabled} onChange={onChange} />
  </Space>
);

export default FlipSwitch;
