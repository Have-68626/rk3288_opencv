import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: string;
  checked: boolean;
  disabled: boolean;
  onChange: (checked: boolean) => void;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({ label, checked, disabled, onChange }) => {
  const id = React.useId();

  const handleLabelClick = (e: React.MouseEvent) => {
    e.preventDefault();
    if (!disabled) {
      onChange(!checked);
    }
  };

  return (
    <label
      htmlFor={id}
      onClick={handleLabelClick}
      style={{
        cursor: disabled ? 'not-allowed' : 'pointer',
        display: 'inline-flex',
        alignItems: 'center',
        gap: 8,
      }}
    >
      <Text disabled={disabled}>{label}</Text>
      <Switch
        id={id}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        onClick={(_, e) => e.stopPropagation()}
      />
    </label>
  );
};

export default FlipSwitch;
