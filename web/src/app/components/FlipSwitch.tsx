import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: string;
  checked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
  checkedChildren?: React.ReactNode;
  unCheckedChildren?: React.ReactNode;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({
  label,
  checked = false,
  disabled = false,
  onChange,
  checkedChildren,
  unCheckedChildren
}) => {
  const id = React.useId();

  return (
    <label
      htmlFor={id}
      onClick={(e) => {
        e.preventDefault();
        if (!disabled) {
          onChange?.(!checked);
        }
      }}
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
        checkedChildren={checkedChildren}
        unCheckedChildren={unCheckedChildren}
      />
    </label>
  );
};

export default FlipSwitch;
