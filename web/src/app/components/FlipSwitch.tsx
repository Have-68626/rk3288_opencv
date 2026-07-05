import { Switch, Space, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: string;
  checked: boolean;
  disabled: boolean;
  onChange: (checked: boolean) => void;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({ label, checked, disabled, onChange }) => {
  const toggle = () => {
    if (!disabled) {
      onChange(!checked);
    }
  };

  return (
    <Space
      onClick={toggle}
      style={{ cursor: disabled ? 'not-allowed' : 'pointer' }}
    >
      <Text disabled={disabled}>{label}</Text>
      <Switch
        checked={checked}
        disabled={disabled}
        onClick={(_, e) => e.stopPropagation()}
        onChange={onChange}
      />
    </Space>
  );
};

export default FlipSwitch;
