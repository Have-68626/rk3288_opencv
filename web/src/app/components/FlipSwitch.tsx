import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: string;
  checked?: boolean;
  disabled?: boolean;
  checkedChildren?: React.ReactNode;
  unCheckedChildren?: React.ReactNode;
  onChange?: (checked: boolean) => void;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({
  label,
  checked,
  disabled,
  checkedChildren,
  unCheckedChildren,
  onChange
}) => {
  const id = React.useId();

  const handleClick = (e: React.MouseEvent<HTMLLabelElement>) => {
    e.preventDefault();
    if (!disabled && onChange) {
      onChange(!checked);
    }
  };

  return (
    <label
      htmlFor={id}
      onClick={handleClick}
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
        checkedChildren={checkedChildren}
        unCheckedChildren={unCheckedChildren}
        onChange={onChange}
        onClick={(_, e) => e.stopPropagation()}
      />
    </label>
  );
};

export default FlipSwitch;
