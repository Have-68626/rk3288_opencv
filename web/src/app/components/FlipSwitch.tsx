import React from 'react';
import { Switch, Typography } from 'antd';
import type { SwitchProps } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps extends SwitchProps {
  label?: string;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({ label, checked, disabled, onChange, ...rest }) => {
  const id = React.useId();

  const handleLabelClick = (e: React.MouseEvent<HTMLLabelElement>) => {
    e.preventDefault();
    if (!disabled && onChange) {
      onChange(!checked, e as unknown as React.MouseEvent<HTMLButtonElement>);
    }
  };

  const switchNode = (
    <Switch
      id={id}
      checked={checked}
      disabled={disabled}
      onChange={(c, e) => {
        if (label) e.stopPropagation();
        if (onChange) onChange(c, e);
      }}
      {...rest}
    />
  );

  if (!label) {
    return switchNode;
  }

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
      {switchNode}
    </label>
  );
};

export default FlipSwitch;
