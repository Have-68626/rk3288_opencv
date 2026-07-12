import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label?: React.ReactNode;
  checked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
  checkedChildren?: React.ReactNode;
  unCheckedChildren?: React.ReactNode;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({
  label,
  checked,
  disabled,
  onChange,
  checkedChildren,
  unCheckedChildren,
}) => {
  const id = React.useId();

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
      {label && <Text disabled={disabled}>{label}</Text>}
      <Switch
        id={id}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        checkedChildren={checkedChildren}
        unCheckedChildren={unCheckedChildren}
        onClick={(_, e) => e.stopPropagation()}
      />
    </label>
  );
};

export default FlipSwitch;
