import React from 'react';
import { Switch, Typography } from 'antd';
import type { SwitchProps } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps extends Omit<SwitchProps, 'onChange'> {
  label?: string;
  checked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({ label, checked, disabled, onChange, ...rest }) => {
  const id = React.useId();

  if (!label) {
    return (
      <Switch
        id={id}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        onClick={(_, e) => e.stopPropagation()}
        {...rest}
      />
    );
  }

  return (
    <label
      htmlFor={id}
      style={{
        cursor: disabled ? 'not-allowed' : 'pointer',
        display: 'inline-flex',
        alignItems: 'center',
        gap: 8,
      }}
      onClick={(e) => {
        e.preventDefault();
        if (!disabled && onChange) {
          onChange(!checked);
        }
      }}
    >
      <Text disabled={disabled}>{label}</Text>
      <Switch
        id={id}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        onClick={(_, e) => e.stopPropagation()}
        {...rest}
      />
    </label>
  );
};

export default FlipSwitch;
